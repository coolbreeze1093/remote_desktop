// rdc — Engine 门面实现。
//
// 线程布局：
//   dispatchThread_  : 引擎派发线程——OnInput/OnState/OnError/OnQualityChanged/
//                      OnRemoteStats 在此串行回调宿主（顺序保证、解耦 rtc 线程）
//   encodeThread_    : Host 编码线程——队列消费 → 转换/缩放 → OpenH264 → RTP
//   decodeThread_    : Viewer 解码线程——AU 队列消费 → OpenH264 解码 → 吐帧
//   statsThread_     : Viewer 每秒统计上传（喂 Host 画质阶梯）+ 停滞检测
//
// 生命周期：所有进入 rtc 回调的 lambda 持有 alive_ 副本，Close() 先翻转标志
// 再关连接、停线程——迟到的回调安全退出（沿用旧 session 的踩坑经验）。
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <rtc/rtc.hpp>

#include "rdc/engine.h"

#include "control_port.h"
#include "control_protocol.h"
#include "frame_queue.h"
#include "media_sender.h"
#include "peer_wrapper.h"
#include "quality_ladder.h"
#include "rdc/log.h"
#include "video_codec.h"
#include "video_convert.h"

namespace rdc {

namespace {

constexpr int kTickFps = 24;  // 采集/节拍频率：编码线程每消费 24 帧评估一次
constexpr int kInitialWidth = 1280;
constexpr int kInitialHeight = 720;
constexpr int kInitialKbps = 4000;
// 编码队列容量（外加正在编码的一帧）：足以扛过 IDR/重配尖峰不丢帧，同时把
// 流水线延迟上限压在 ~3 帧间隔、BGRA 内存控制在几帧缓冲内。
constexpr size_t kEncQueueCapacity = 3;
// 解码队列容量：解码是轻活，16 个 AU 足够吸收抖动；满则丢最旧。
constexpr size_t kDecodeQueueCapacity = 16;

std::vector<std::string> DefaultIceServers() {
  // STUN 只在跨 NAT 时有用；它还会产生 srflx 候选，同机/局域网会话可能选中
  // NAT 回程路径然后静默丢包。RD_NO_STUN=1 禁用（诊断用）。
  if (std::getenv("RD_NO_STUN") != nullptr) {
    return {};
  }
  return {"stun:stun.l.google.com:19302"};
}

uint64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 编码队列任务：BGRA 或 I420 源，编码线程负责转换/缩放到编码器当前尺寸。
struct EncodeJob {
  bool isI420 = false;
  int width = 0;
  int height = 0;
  int stride = 0;  // BGRA 行距
  std::vector<uint8_t> bgra;
  std::vector<uint8_t> y, u, v;  // I420 直推（紧凑平面）
};

// 解码输出：平面缓冲与视图打包在一起，shared_ptr 引用计数保活。
struct DecodedFrame {
  VideoFrame view;
  std::vector<uint8_t> y, u, v;
};

class EngineImpl : public Engine {
 public:
  EngineImpl(Role role, Config cfg)
      : role_(role), cfg_(std::move(cfg)),
        encQueue_(kEncQueueCapacity), decQueue_(kDecodeQueueCapacity) {
    // 测试后门（与旧实现等价）：
    //   RD_TEST_NO_PEER_SESSION=1 → 只跑采集/编码流水线，不建 PeerConnection
    //   RD_TEST_NO_ENC=1          → 只跑采集/队列交接，不初始化编码器
    testSkipPeer_ = std::getenv("RD_TEST_NO_PEER_SESSION") != nullptr;
    testSkipEncode_ = std::getenv("RD_TEST_NO_ENC") != nullptr;
    if (cfg_.iceServers.empty()) {
      cfg_.iceServers = DefaultIceServers();
    }
  }

  ~EngineImpl() override { Close(); }

  // ---------------------------------------------------------------- Start

  bool Start() override {
    std::lock_guard<std::mutex> lock(startMtx_);
    if (started_) {
      return true;
    }
    dispatchRunning_ = true;
    dispatchThread_ = std::thread([this] { DispatchLoop(); });

    // ---- 控制通道回调（必须先于 CreateOn/AcceptOn 设置）----
    const auto alive = alive_;
    control_.SetCallbacks(
        /*onOpen*/
        [this, alive] {
          if (!alive->load(std::memory_order_acquire)) {
            return;
          }
          // Viewer：通道打开即同步一次画质选择（连接建立时机可能早于通道）。
          if (role_ == Role::Viewer) {
            control_.Send(ctrl::EncodeQualityMode(
                qualityMode_.load(std::memory_order_relaxed)));
          }
        },
        /*onClosed*/ [] {},
        /*onError*/ [](const std::string&) {},
        /*onMessage*/
        [this, alive](const ctrl::Message& m) {
          if (!alive->load(std::memory_order_acquire)) {
            return;
          }
          HandleControlMessage(m);
        });

    // ---- PeerConnection 状态回调 ----
    auto onState = [this, alive](rtc::PeerConnection::State st) {
      if (!alive->load(std::memory_order_acquire)) {
        return;
      }
      switch (st) {
        case rtc::PeerConnection::State::New:
          PostState(State::Idle, "new");
          break;
        case rtc::PeerConnection::State::Connecting:
          PostState(State::Connecting, "connecting");
          break;
        case rtc::PeerConnection::State::Connected:
          if (role_ == Role::Host) {
            keyframeRequested_ = true;  // (重)连：强制 IDR
          }
          PostState(State::Connected, "connected");
          break;
        case rtc::PeerConnection::State::Disconnected:
          PostState(State::Closed, "disconnected");
          break;
        case rtc::PeerConnection::State::Failed:
          PostState(State::Failed, "连接失败");
          break;
        case rtc::PeerConnection::State::Closed:
          PostState(State::Closed, "closed");
          break;
      }
    };

    bool ok = false;
    if (role_ == Role::Host) {
      ok = StartHost(onState);
    } else {
      ok = StartViewer(onState);
    }
    if (!ok) {
      // 回滚已启动的部分线程，保持对象可重试/可析构。
      started_ = false;
      ShutdownThreads();
      return false;
    }
    started_ = true;
    return true;
  }

  // ---------------------------------------------------------------- 信令

  std::string CreateOffer() override {
    if (role_ != Role::Viewer) {
      return {};
    }
    std::string error;
    const std::string sdp = peer_.CreateOfferBlocking(&error);
    if (sdp.empty() && !error.empty()) {
      PostError(error);
    }
    return sdp;
  }

  std::string AcceptOfferCreateAnswer(const std::string& offerSdp) override {
    if (role_ != Role::Host) {
      return {};
    }
    std::string error;
    const std::string sdp =
        peer_.AcceptOfferCreateAnswerBlocking(offerSdp, &error);
    if (sdp.empty() && !error.empty()) {
      PostError(error);
    }
    return sdp;
  }

  bool AcceptAnswer(const std::string& answerSdp) override {
    if (role_ != Role::Viewer) {
      return false;
    }
    std::string error;
    if (!peer_.AcceptAnswer(answerSdp, &error)) {
      PostError(error.empty() ? "answer rejected" : error);
      return false;
    }
    return true;
  }

  // ---------------------------------------------------------------- 视频

  bool PushVideoFrameBgra(const uint8_t* bgra, int strideBytes, int width,
                          int height) override {
    if (!bgra || width <= 0 || height <= 0 ||
        !alive_->load(std::memory_order_acquire)) {
      return false;
    }
    EncodeJob job;
    job.isI420 = false;
    job.width = width;
    job.height = height;
    job.stride = strideBytes > 0 ? strideBytes : width * 4;
    job.bgra.resize(static_cast<size_t>(job.stride) * height);
    std::memcpy(job.bgra.data(), bgra, job.bgra.size());
    return PushJob(std::move(job));
  }

  bool PushVideoFrameI420(const uint8_t* y, int yStride, const uint8_t* u,
                          int uStride, const uint8_t* v, int vStride,
                          int width, int height) override {
    if (!y || !u || !v || width <= 0 || height <= 0 ||
        !alive_->load(std::memory_order_acquire)) {
      return false;
    }
    EncodeJob job;
    job.isI420 = true;
    job.width = width;
    job.height = height;
    const size_t yRow = static_cast<size_t>(width);
    const size_t uvRow = static_cast<size_t>(width / 2);
    const size_t chromaRows = static_cast<size_t>((height + 1) / 2);
    job.y.resize(yRow * height);
    job.u.resize(uvRow * chromaRows);
    job.v.resize(uvRow * chromaRows);
    for (int r = 0; r < height; ++r) {
      std::memcpy(job.y.data() + static_cast<size_t>(r) * yRow,
                  y + static_cast<size_t>(r) * yStride, yRow);
    }
    for (size_t r = 0; r < chromaRows; ++r) {
      std::memcpy(job.u.data() + r * uvRow,
                  u + r * static_cast<size_t>(uStride), uvRow);
      std::memcpy(job.v.data() + r * uvRow,
                  v + r * static_cast<size_t>(vStride), uvRow);
    }
    return PushJob(std::move(job));
  }

  void OnVideoFrame(
      std::function<void(std::shared_ptr<const VideoFrame>)> cb) override {
    std::lock_guard<std::mutex> lock(cbMtx_);
    videoCb_ = std::move(cb);
  }

  // ---------------------------------------------------------------- 输入

  void SendInput(const InputEvent& ev) override {
    InputEvent out = ev;
    out.timestampUs = NowUs();
    static std::atomic<int> dbgSend{0};
    if (dbgSend.fetch_add(1) < 5) {
      rdcDebug("ctrl", "SendInput type=%d dcOpen=%d", static_cast<int>(out.type),
               (int)control_.IsOpen());
    }
    control_.Send(ctrl::EncodeInputEvent(out));
  }

  void OnInput(std::function<void(const InputEvent&)> cb) override {
    std::lock_guard<std::mutex> lock(cbMtx_);
    inputCb_ = std::move(cb);
  }

  // -------------------------------------------------------- 安全桌面状态

  void SendSecureDesktopState(bool secure, bool capturable) override {
    static std::atomic<int> dbgSd{0};
    if (dbgSd.fetch_add(1) < 5) {
      rdcDebug("ctrl", "SendSecureDesktopState secure=%d capturable=%d dcOpen=%d",
               (int)secure, (int)capturable, (int)control_.IsOpen());
    }
    control_.Send(ctrl::EncodeSecureDesktopState(secure, capturable));
  }

  void OnSecureDesktop(
      std::function<void(bool secure, bool capturable)> cb) override {
    std::lock_guard<std::mutex> lock(cbMtx_);
    secureCb_ = std::move(cb);
  }

  // ------------------------------------------------------------ 事件/画质

  void OnState(
      std::function<void(State, const std::string&)> cb) override {
    std::lock_guard<std::mutex> lock(cbMtx_);
    stateCb_ = std::move(cb);
  }

  void OnError(std::function<void(const std::string&)> cb) override {
    std::lock_guard<std::mutex> lock(cbMtx_);
    errorCb_ = std::move(cb);
  }

  void OnQualityChanged(
      std::function<void(int, int, int, int, int)> cb) override {
    std::lock_guard<std::mutex> lock(cbMtx_);
    qualityCb_ = std::move(cb);
  }

  void OnRemoteStats(std::function<void(int, int, int)> cb) override {
    std::lock_guard<std::mutex> lock(cbMtx_);
    remoteStatsCb_ = std::move(cb);
  }

  void SetQualityMode(int mode) override {
    qualityMode_.store(mode, std::memory_order_relaxed);
    rdcDebug("quality", "mode set to %d (role=%s)", mode,
             role_ == Role::Host ? "host" : "viewer");
    if (role_ == Role::Viewer && control_.IsOpen()) {
      control_.Send(ctrl::EncodeQualityMode(mode));
    }
  }

  Stats GetStats() const override {
    Stats s;
    s.captured = captured_.load(std::memory_order_relaxed);
    s.encoded = encoded_.load(std::memory_order_relaxed);
    s.sent = sent_.load(std::memory_order_relaxed);
    s.dropped = dropped_.load(std::memory_order_relaxed);
    s.received = received_.load(std::memory_order_relaxed);
    s.decoded = decoded_.load(std::memory_order_relaxed);
    s.decodeFailed = decodeFailed_.load(std::memory_order_relaxed);
    s.qualityLevel = qualityLevel_.load(std::memory_order_relaxed);
    s.encodeWidth = encodeW_.load(std::memory_order_relaxed);
    s.encodeHeight = encodeH_.load(std::memory_order_relaxed);
    s.encodeMs = static_cast<int>(encodeEmaMs_.load(std::memory_order_relaxed));
    return s;
  }

  // ---------------------------------------------------------------- Close

  void Close() override {
    if (!alive_->exchange(false, std::memory_order_release)) {
      return;  // 幂等
    }
    // 顺序：先断流量（DC/PC），再停线程——rtc 回调线程在连接关闭后排空，
    // 之后线程退出，最后清对象。先关线程再关连接曾是 use-after-free。
    control_.Close();
    peer_.Close();
    ShutdownThreads();
    { std::lock_guard<std::mutex> lock(cbMtx_); videoCb_ = nullptr; inputCb_ = nullptr; stateCb_ = nullptr; errorCb_ = nullptr; qualityCb_ = nullptr; remoteStatsCb_ = nullptr; }
    started_ = false;
  }

 private:
  // ------------------------------------------------------------ 启动/关闭

  bool StartHost(const PeerWrapper::StateFn& onState) {
    if (!testSkipPeer_) {
      auto onTrack = [this, alive = alive_](std::shared_ptr<rtc::Track> track) {
        if (!alive->load(std::memory_order_acquire)) {
          return;
        }
        if (!track || track->description().type() != "video") {
          return;
        }
        const int dir = static_cast<int>(track->description().direction());
        rdcDebug("session", "host onTrack video dir=%d", dir);
        // libdatachannel 在持有内部锁的状态下派发 onTrack；在回调里直接
        // setMediaHandler 会自锁（shared_mutex EDEADLK，旧实现同样靠把
        // attach 跳出回调线程规避）。转投派发线程完成挂接。
        PostTask([this, alive, track] {
          if (!alive->load(std::memory_order_acquire)) {
            return;
          }
          rdcDebug("session", "host attach sender open=%d",
                   (int)track->isOpen());
          sender_.attach(track, [this, alive] {
            if (!alive->load(std::memory_order_acquire)) {
              return;
            }
            keyframeRequested_ = true;  // 接收方要 IDR（PLI/FIR）
          });
          keyframeRequested_ = true;  // 解码端需要 IDR 才能起播
        });
      };
      std::string error;
      if (!peer_.Start(/*isOfferer=*/false, /*sendVideo=*/false,
                       cfg_.iceServers, onState, std::move(onTrack))) {
        PostError(error.empty() ? "peer start failed" : error);
        return false;
      }
      control_.AcceptOn(peer_.connection());
    }
    keyframeRequested_ = true;
    qualityLevel_ = QualityLadder::kLevels - 1;  // auto 从最清晰起步
    if (!testSkipEncode_) {
      encoder_ = std::make_unique<H264Encoder>();
      if (!encoder_->init(kInitialWidth, kInitialHeight, kTickFps,
                          kInitialKbps)) {
        PostError("编码器初始化失败");
        return false;
      }
    }
    encodeW_ = kInitialWidth;
    encodeH_ = kInitialHeight;
    encodeRunning_ = true;
    encodeThread_ = std::thread([this] { EncodeLoop(); });
    return true;
  }

  bool StartViewer(const PeerWrapper::StateFn& onState) {
    std::string error;
    if (!peer_.Start(/*isOfferer=*/true, /*sendVideo=*/false, cfg_.iceServers,
                     onState, /*onTrack=*/nullptr)) {
      PostError(error.empty() ? "peer start failed" : error);
      return false;
    }
    control_.CreateOn(peer_.connection());

    decoder_ = std::make_unique<H264Decoder>();
    if (!decoder_->init()) {
      rdcDebug("session", "viewer decoder init FAILED");
      PostError("解码器初始化失败");
      return false;
    }

    // 预加的 RecvOnly 轨道就是收流轨道（既有 mid 不触发 onTrack）。
    std::shared_ptr<rtc::Track> track = peer_.localTrack();
    if (!track) {
      PostError("视频轨道创建失败");
      return false;
    }
    track->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>());
    track->onOpen([] {
      rdcDebug("session", "viewer track open");
    });
    // 解包后的视频帧带 FrameInfo，走 onFrame；onMessage 会静默丢弃它们
    // （libdatachannel >= 0.20）。
    const auto alive = alive_;
    track->onFrame([this, alive](rtc::binary data,
                                 [[maybe_unused]] rtc::FrameInfo info) {
      if (!alive->load(std::memory_order_acquire)) {
        return;
      }
      received_.fetch_add(1, std::memory_order_relaxed);
      static std::atomic<int> dbgCount{0};
      if (dbgCount.fetch_add(1) < 3) {
        rdcDebug("session", "viewer received frame #%d size=%zu",
                 dbgCount.load(), data.size());
      }
      if (data.empty()) {
        return;
      }
      std::vector<uint8_t> au(data.size());
      std::memcpy(au.data(),
                  reinterpret_cast<const uint8_t*>(data.data()), au.size());
      const size_t dropped =
          decQueue_.Push(std::move(au));  // 满则丢最旧，延迟不累积
      if (dropped > 0) {
        decDropped_.fetch_add(dropped, std::memory_order_relaxed);
      }
    });
    decodeRunning_ = true;
    decodeThread_ = std::thread([this] { DecodeLoop(); });
    statsRunning_ = true;
    statsThread_ = std::thread([this] { StatsLoop(); });
    return true;
  }

  void ShutdownThreads() {
    encodeRunning_ = false;
    encQueue_.Stop();
    if (encodeThread_.joinable()) {
      encodeThread_.join();
    }
    decodeRunning_ = false;
    decQueue_.Stop();
    if (decodeThread_.joinable()) {
      decodeThread_.join();
    }
    statsRunning_ = false;
    {
      std::lock_guard<std::mutex> lock(statsMtx_);
    }
    statsCv_.notify_all();
    if (statsThread_.joinable()) {
      statsThread_.join();
    }
    dispatchRunning_ = false;
    {
      std::lock_guard<std::mutex> lock(dispatchMtx_);
      dispatchQ_.clear();  // 停机时丢弃未派发任务，避免 Close 后回调宿主
    }
    dispatchCv_.notify_all();
    if (dispatchThread_.joinable()) {
      dispatchThread_.join();
    }
    // 编/解码器归所属线程独占；线程 join 完成后再销毁。
    if (encoder_) {
      encoder_->close();
      encoder_.reset();
    }
    if (decoder_) {
      decoder_->close();
      decoder_.reset();
    }
  }

  // ------------------------------------------------------------ 派发线程

  void PostTask(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(dispatchMtx_);
      if (!dispatchRunning_.load(std::memory_order_relaxed)) {
        return;
      }
      dispatchQ_.push_back(std::move(task));
    }
    dispatchCv_.notify_one();
  }

  void DispatchLoop() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(dispatchMtx_);
        dispatchCv_.wait(lock, [this] {
          return !dispatchRunning_.load(std::memory_order_relaxed) ||
                 !dispatchQ_.empty();
        });
        if (!dispatchRunning_.load(std::memory_order_relaxed)) {
          return;  // 停机：剩余任务按约定丢弃
        }
        task = std::move(dispatchQ_.front());
        dispatchQ_.pop_front();
      }
      task();
    }
  }

  void DeliverVideoFrame(std::shared_ptr<const VideoFrame> frame) {
    std::function<void(std::shared_ptr<const VideoFrame>)> cb;
    {
      std::lock_guard<std::mutex> lock(cbMtx_);
      cb = videoCb_;
    }
    if (cb) {
      cb(std::move(frame));
    }
  }

  // -------------------------------------------------------- 控制消息处理

  // libdatachannel 线程。
  void HandleControlMessage(const ctrl::Message& m) {
    switch (m.type) {
      case ctrl::Type::Input: {
        static std::atomic<int> dbgIn{0};
        if (dbgIn.fetch_add(1) < 5) {
          rdcDebug("ctrl", "input msg #%d arrived", dbgIn.load());
        }
        InputEvent ev = m.input;
        ev.timestampUs = NowUs();
        PostTask([this, ev] { DeliverInput(ev); });
        break;
      }
      case ctrl::Type::FrameMeta:
        // Viewer 端诊断：显示 Host 当前编码尺寸。
        encodeW_.store(m.metaWidth, std::memory_order_relaxed);
        encodeH_.store(m.metaHeight, std::memory_order_relaxed);
        break;
      case ctrl::Type::VideoStats:
        if (role_ == Role::Host) {
          // 喂画质阶梯（编码线程每秒 drain），同时通知观测回调。
          statRecv_.fetch_add(static_cast<int>(m.statsReceived),
                              std::memory_order_relaxed);
          statDec_.fetch_add(static_cast<int>(m.statsDecoded),
                             std::memory_order_relaxed);
          statFail_.fetch_add(static_cast<int>(m.statsFailed),
                              std::memory_order_relaxed);
          const int r = static_cast<int>(m.statsReceived);
          const int d = static_cast<int>(m.statsDecoded);
          const int f = static_cast<int>(m.statsFailed);
          PostTask([this, r, d, f] {
            std::function<void(int, int, int)> cb;
            {
              std::lock_guard<std::mutex> lock(cbMtx_);
              cb = remoteStatsCb_;
            }
            if (cb) {
              cb(r, d, f);
            }
          });
        }
        break;
      case ctrl::Type::QualityMode:
        // Host 端收到 Viewer 的画质选择；编码线程下一次 tick 生效。
        qualityMode_.store(m.qualityMode, std::memory_order_relaxed);
        rdcDebug("quality", "remote mode set to %d",
                 static_cast<int>(m.qualityMode));
        break;
      case ctrl::Type::KeyframeReq:
        keyframeRequested_ = true;
        break;
      case ctrl::Type::SecureDesktop: {
        // Viewer 端：宿主锁屏状态变化 → 派发线程回调（顺序与发送方一致）。
        const bool secure = m.secureDesktop;
        const bool capturable = m.secureCapturable;
        PostTask([this, secure, capturable] {
          std::function<void(bool, bool)> cb;
          {
            std::lock_guard<std::mutex> lock(cbMtx_);
            cb = secureCb_;
          }
          if (cb) {
            cb(secure, capturable);
          }
        });
        break;
      }
      default:
        break;
    }
  }

  void DeliverInput(const InputEvent& ev) {
    std::function<void(const InputEvent&)> cb;
    {
      std::lock_guard<std::mutex> lock(cbMtx_);
      cb = inputCb_;
    }
    if (cb) {
      cb(ev);
    }
  }

  void PostState(State state, std::string detail) {
    PostTask([this, state, detail = std::move(detail)] {
      std::function<void(State, const std::string&)> cb;
      {
        std::lock_guard<std::mutex> lock(cbMtx_);
        cb = stateCb_;
      }
      if (cb) {
        cb(state, detail);
      }
    });
  }

  void PostError(std::string detail) {
    rdcDebug("session", "error: %s", detail.c_str());
    PostTask([this, detail = std::move(detail)] {
      std::function<void(const std::string&)> cb;
      {
        std::lock_guard<std::mutex> lock(cbMtx_);
        cb = errorCb_;
      }
      if (cb) {
        cb(detail);
      }
    });
  }

  // ------------------------------------------------------------ 编码线程

  bool PushJob(EncodeJob job) {
    captured_.fetch_add(1, std::memory_order_relaxed);
    const size_t dropped = encQueue_.Push(std::move(job));
    if (dropped > 0) {
      const uint32_t total =
          dropped_.fetch_add(static_cast<uint32_t>(dropped),
                             std::memory_order_relaxed) +
          static_cast<uint32_t>(dropped);
      if (total % kTickFps == 1) {  // 每秒至多一行
        rdcDebug("encode", "encode queue full: dropped total=%u", total);
      }
    }
    return true;
  }

  void EncodeLoop() {
    rdcDebug("session", "encode loop enter");
    int tickFrames = 0;
    int encodedThisTick = 0;
    int fpsAccumulator = 0;
    int configuredFps = 0;

    while (encodeRunning_.load(std::memory_order_relaxed)) {
      EncodeJob job;
      if (!encQueue_.PopWait(job)) {
        break;
      }
      if (!encoder_) {
        continue;  // RD_TEST_NO_ENC：只测采集/队列交接
      }

      const auto t0 = std::chrono::steady_clock::now();
      quality_.setNativeSize(job.width, job.height);

      // 每秒一次：drain Viewer 统计 → 画质阶梯评估 → 需要则重配编码器。
      // tick 计数按交给编码器的帧算；队列丢帧只发生在编码器瓶颈时，
      // 负载下只是拉长 tick 周期。
      if (++tickFrames >= kTickFps) {
        tickFrames = 0;
        const int recv = statRecv_.exchange(0, std::memory_order_relaxed);
        const int dec = statDec_.exchange(0, std::memory_order_relaxed);
        const int fail = statFail_.exchange(0, std::memory_order_relaxed);
        if (recv | dec | fail) {
          quality_.onViewerStats(recv, dec, fail);
        }
        const int desired =
            quality_.evaluate(qualityMode_.load(std::memory_order_relaxed),
                              encodedThisTick);
        encodedThisTick = 0;
        qualityLevel_.store(quality_.level(), std::memory_order_relaxed);
        int w = 0, h = 0;
        quality_.sizeFor(desired, &w, &h);
        const int kbps = QualityLadder::bitrateKbpsFor(desired);
        const int fps = QualityLadder::fpsFor(desired);
        if (w != encoder_->width() || h != encoder_->height() ||
            kbps != encoder_->bitrateKbps() || fps != configuredFps) {
          if (encoder_->reconfigure(w, h, fps, kbps)) {
            configuredFps = fps;
            keyframeRequested_ = true;  // 解码端需要新 SPS/PPS
            encodeW_.store(w, std::memory_order_relaxed);
            encodeH_.store(h, std::memory_order_relaxed);
            if (control_.IsOpen()) {
              control_.Send(ctrl::EncodeFrameMeta(w, h));  // Viewer 诊断
            }
            const int level = desired;
            PostTask([this, level, w, h, fps, kbps] {
              std::function<void(int, int, int, int, int)> cb;
              {
                std::lock_guard<std::mutex> lock(cbMtx_);
                cb = qualityCb_;
              }
              if (cb) {
                cb(level, w, h, fps, kbps);
              }
            });
            rdcDebug("quality", "encoder reconfigured to %dx%d @%dkbps %dfps",
                     w, h, kbps, fps);
          }
        }
      }

      // 帧率门控：按当前档位的 fps 编码（累加器让丢帧均匀分布；采集保持
      // kTickFps）。
      fpsAccumulator += QualityLadder::fpsFor(quality_.level());
      if (fpsAccumulator < kTickFps) {
        continue;
      }
      fpsAccumulator -= kTickFps;

      bool forceKey = keyframeRequested_.exchange(false);
      if (sender_.consumeOpenTransition()) {
        // track 刚转 open：解码端可能中途入会，从一个 IDR 起流。
        forceKey = true;
      }
      // 按编码器当前尺寸转换（阶梯可能在帧间重配；encode() 也对尺寸失配
      // 有保护）。
      I420Buffer i420 =
          job.isI420
              ? ScaleI420(job.y.data(), job.width, job.u.data(),
                          job.width / 2, job.v.data(), job.width / 2,
                          job.width, job.height, encoder_->width(),
                          encoder_->height())
              : ConvertBgraToI420(job.bgra.data(), job.stride, job.width,
                                  job.height, encoder_->width(),
                                  encoder_->height());
      std::vector<uint8_t> au = encoder_->encode(i420, forceKey);
      const auto t1 = std::chrono::steady_clock::now();
      quality_.onEncodedFrame(
          std::chrono::duration<double, std::milli>(t1 - t0).count());
      encodeEmaMs_.store(static_cast<float>(quality_.emaMs()),
                         std::memory_order_relaxed);
      if (!au.empty()) {
        ++encodedThisTick;
        encoded_.fetch_add(1, std::memory_order_relaxed);
        sender_.send(au, forceKey);
        sent_.fetch_add(1, std::memory_order_relaxed);
      }
    }
    // 停机顺序：encode 线程退出即代表不再消费；采集线程由宿主先行 stop。
    rdcDebug("session", "encode loop exit");
  }

  // ------------------------------------------------------------ 解码线程

  void DecodeLoop() {
    while (decodeRunning_.load(std::memory_order_relaxed)) {
      std::vector<uint8_t> au;
      if (!decQueue_.PopWait(au)) {
        break;
      }
      if (!decoder_) {
        continue;
      }
      I420Buffer buf;
      int yStride = 0, uvStride = 0;
      if (!decoder_->decode(au.data(), au.size(), buf, &yStride, &uvStride)) {
        decodeFailed_.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> dbgFails{0};
        if (dbgFails.fetch_add(1) < 3) {
          rdcDebug("session", "viewer decode failed #%d (size=%zu)",
                   dbgFails.load(), au.size());
        }
        continue;
      }
      decoded_.fetch_add(1, std::memory_order_relaxed);
      // 每 ~120 帧（5s）一次心跳，让停滞阶段在日志里可见。
      if (received_.load(std::memory_order_relaxed) % 120 == 1) {
        rdcDebug("session", "viewer diag received=%u decoded=%u failed=%u",
                 received_.load(), decoded_.load(), decodeFailed_.load());
      }
      auto frame = std::make_shared<DecodedFrame>();
      frame->y = std::move(buf.y);
      frame->u = std::move(buf.u);
      frame->v = std::move(buf.v);
      frame->view.width = buf.width;
      frame->view.height = buf.height;
      frame->view.y = frame->y.data();
      frame->view.u = frame->u.data();
      frame->view.v = frame->v.data();
      frame->view.yStride = yStride;
      frame->view.uStride = uvStride;
      frame->view.vStride = uvStride;
      frame->view.timestampUs = NowUs();
      std::shared_ptr<const VideoFrame> view(frame, &frame->view);
      DeliverVideoFrame(std::move(view));
    }
  }

  // ------------------------------------------------------------ 统计线程

  // Viewer：每秒上传解码统计增量（喂 Host 画质阶梯）+ 停滞检测。
  void StatsLoop() {
    uint32_t lastRecv = 0, lastDec = 0, lastFail = 0;
    uint32_t stallLastRecv = 0;
    int stallTicks = 0;
    std::unique_lock<std::mutex> lock(statsMtx_);
    while (statsRunning_.load(std::memory_order_relaxed)) {
      statsCv_.wait_for(lock, std::chrono::seconds(1), [this] {
        return !statsRunning_.load(std::memory_order_relaxed);
      });
      if (!statsRunning_.load(std::memory_order_relaxed)) {
        break;
      }
      if (!control_.IsOpen()) {
        continue;
      }
      const uint32_t r = received_.load(std::memory_order_relaxed);
      const uint32_t d = decoded_.load(std::memory_order_relaxed);
      const uint32_t f = decodeFailed_.load(std::memory_order_relaxed);
      control_.Send(ctrl::EncodeVideoStats(r - lastRecv, d - lastDec,
                                           f - lastFail));
      lastRecv = r;
      lastDec = d;
      lastFail = f;
      // 停滞检测：连接已建立时 received 必须持续推进；停住说明画面冻结且
      // 日志无因——把停滞本身暴露出来。
      if (r != stallLastRecv) {
        stallLastRecv = r;
        stallTicks = 0;
      } else if (++stallTicks == 3) {
        rdcDebug("session",
                 "viewer STALL: received static at %u for 3s (decoded=%u "
                 "failed=%u)",
                 r, d, f);
      }
    }
  }

  // ---------------------------------------------------------------- 成员

  Role role_;
  Config cfg_;
  bool testSkipPeer_ = false;
  bool testSkipEncode_ = false;
  bool started_ = false;
  std::mutex startMtx_;

  // 生存守卫：每个 rtc 回调 lambda 持有副本；Close 先置 false。
  std::shared_ptr<std::atomic_bool> alive_ =
      std::make_shared<std::atomic_bool>(true);

  PeerWrapper peer_;
  MediaSender sender_;
  ControlPort control_;

  // 编码侧（Host）
  DropOldestQueue<EncodeJob> encQueue_;
  std::thread encodeThread_;
  std::atomic_bool encodeRunning_{false};
  std::unique_ptr<H264Encoder> encoder_;  // 编码线程独占
  QualityLadder quality_;                 // 编码线程独占

  // 解码侧（Viewer）
  DropOldestQueue<std::vector<uint8_t>> decQueue_;  // AU 队列
  std::thread decodeThread_;
  std::atomic_bool decodeRunning_{false};
  std::unique_ptr<H264Decoder> decoder_;  // 解码线程独占
  std::atomic<uint32_t> decDropped_{0};

  // Viewer 每秒统计线程
  std::thread statsThread_;
  std::atomic_bool statsRunning_{false};
  std::mutex statsMtx_;
  std::condition_variable statsCv_;

  // 派发线程
  std::thread dispatchThread_;
  std::atomic_bool dispatchRunning_{false};
  std::mutex dispatchMtx_;
  std::condition_variable dispatchCv_;
  std::deque<std::function<void()>> dispatchQ_;

  // 宿主回调（cbMtx_ 保护设置/读取）
  std::mutex cbMtx_;
  std::function<void(std::shared_ptr<const VideoFrame>)> videoCb_;
  std::function<void(const InputEvent&)> inputCb_;
  std::function<void(bool secure, bool capturable)> secureCb_;
  std::function<void(State, const std::string&)> stateCb_;
  std::function<void(const std::string&)> errorCb_;
  std::function<void(int, int, int, int, int)> qualityCb_;
  std::function<void(int, int, int)> remoteStatsCb_;

  // 跨线程诊断计数（采集线程 / rtc 线程 / 宿主线程）
  std::atomic<uint32_t> captured_{0}, encoded_{0}, sent_{0}, dropped_{0};
  std::atomic<uint32_t> received_{0}, decoded_{0}, decodeFailed_{0};
  std::atomic<int> qualityMode_{0};
  std::atomic<int> statRecv_{0}, statDec_{0}, statFail_{0};
  std::atomic<int> qualityLevel_{-1};
  std::atomic<int> encodeW_{0}, encodeH_{0};
  std::atomic<float> encodeEmaMs_{0};
  std::atomic<bool> keyframeRequested_{true};
};

}  // namespace

std::unique_ptr<Engine> Engine::Create(Role role, Config cfg) {
  return std::unique_ptr<Engine>(new EngineImpl(role, std::move(cfg)));
}

}  // namespace rdc
