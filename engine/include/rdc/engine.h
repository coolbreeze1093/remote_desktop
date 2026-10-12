// rdc — 纯 C++17 远程桌面 WebRTC 引擎（无 Qt、无平台 UI 依赖）。
//
// 对外只暴露一个门面类 Engine：宿主程序通过 API 推帧/发输入事件，通过回调
// 收解码帧/收输入事件/收连接状态。传输底座是 libdatachannel（ICE/DTLS-SRTP/
// SCTP），编解码是 OpenH264，都封装在引擎内部。
//
// 回调线程契约（务必遵守）：
//   - OnVideoFrame      ：引擎解码线程，单线程串行；持有 shared_ptr 即可
//                         带到任意线程使用。回调应轻；重活自行转投线程。
//   - OnInput           ：引擎派发线程（与发送方顺序一致）。
//   - OnState/OnError 等：引擎派发线程。
//   - Engine 的所有方法：任意线程可调（内部加锁）。
//   - 回调内禁止同步调用 CreateOffer/AcceptOfferCreateAnswer 等阻塞信令方法
//     （会死锁）；引擎在调试模式下对此路径有断言防护。
#ifndef RDC_ENGINE_H_
#define RDC_ENGINE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rdc {

enum class Role { Host, Viewer };

// 粗粒度状态机：宿主据此更新 UI / 日志。
enum class State { Idle, Gathering, Connecting, Connected, Failed, Closed };

// ---------- 数据对象 ----------

// I420 视频帧视图。回调交付 shared_ptr<const VideoFrame>，平面内存由引用计数
// 保活；宿主拿到后可带到任意线程，零拷贝。
struct VideoFrame {
  int width = 0, height = 0;
  const uint8_t* y = nullptr;
  int yStride = 0;
  const uint8_t* u = nullptr;
  int uStride = 0;
  const uint8_t* v = nullptr;
  int vStride = 0;
  uint64_t timestampUs = 0;
};

// 输入事件（归一化坐标 [0..1]，相对远端画面）。
struct InputEvent {
  enum class Type : uint8_t { MouseMove = 0, MouseButton, MouseWheel, Key };
  Type type = Type::MouseMove;
  double nx = 0, ny = 0;
  int button = 0;         // 1=左 2=右 3=中
  bool pressed = false;
  int wheelDelta = 0;     // 每格 ±120
  uint32_t keyCode = 0;   // Windows VK 码
  bool extended = false;
  uint64_t timestampUs = 0;  // 引擎收包时刻（Host 端诊断用）
};

struct Config {
  // 为空时取默认 stun:stun.l.google.com:19302（环境变量 RD_NO_STUN=1 可禁用）。
  std::vector<std::string> iceServers;
};

// 诊断统计：口径与旧 rd::RemoteDesktopSession::DiagStats 完全一致。
// Host 关注 captured/encoded/sent/dropped/quality*；Viewer 关注
// received/decoded/decodeFailed 与 encodeWidth/Height（来自 FrameMeta）。
struct Stats {
  uint32_t captured = 0;
  uint32_t encoded = 0;
  uint32_t sent = 0;
  uint32_t dropped = 0;
  uint32_t received = 0;
  uint32_t decoded = 0;
  uint32_t decodeFailed = 0;
  int qualityLevel = -1;  // 0=流畅 1=高清 2=超清；Viewer 端无意义
  int encodeWidth = 0;
  int encodeHeight = 0;
  int encodeMs = 0;       // Host：每帧转换+编码+发送耗时 EMA
};

// ---------- 工具：I420 -> BGRA（供宿主渲染；libyuv 实现） ----------
// 输出为内存序 BGRA（libyuv 的 "ARGB" 小端布局）。bgraStrideBytes 为目标行距。
void I420ToBgra(const uint8_t* y, int yStride, const uint8_t* u, int uStride,
                const uint8_t* v, int vStride, int width, int height,
                uint8_t* bgraOut, int bgraStrideBytes);

// ---------- 引擎门面 ----------
class Engine {
 public:
  virtual ~Engine() = default;

  static std::unique_ptr<Engine> Create(Role role, Config cfg = {});

  // ---- 信令（非 trickle，手动交换 SDP；阻塞至 ICE 收集完成，上限 10s）----
  // Start 建立内部线程与（除非测试环境变量豁免）PeerConnection/DataChannel。
  // Host 端编码器在这里初始化；失败返回 false 并经 OnError 报告。
  virtual bool Start() = 0;
  virtual std::string CreateOffer() = 0;  // Viewer：返回完整 Offer SDP
  virtual std::string AcceptOfferCreateAnswer(
      const std::string& offerSdp) = 0;   // Host：返回完整 Answer SDP
  virtual bool AcceptAnswer(const std::string& answerSdp) = 0;  // Viewer
  virtual void Close() = 0;               // 幂等；之后对象可安全析构

  // ---- 视频：Host 推帧 ----
  // BGRA 进（引擎内 libyuv 转/缩放 I420 后编码）。非阻塞：内部有界队列(~3)，
  // 满时丢最旧帧并计入 dropped，采集线程永不因编码慢而卡住。
  virtual bool PushVideoFrameBgra(const uint8_t* bgra, int strideBytes,
                                  int width, int height) = 0;
  // 已是 I420 时直推（平面行距任意，引擎内部完成缩放/拷贝）。
  virtual bool PushVideoFrameI420(const uint8_t* y, int yStride,
                                  const uint8_t* u, int uStride,
                                  const uint8_t* v, int vStride,
                                  int width, int height) = 0;

  // ---- 视频：Viewer 吐帧（解码线程串行回调）----
  virtual void OnVideoFrame(
      std::function<void(std::shared_ptr<const VideoFrame>)> cb) = 0;

  // ---- 输入 ----
  virtual void SendInput(const InputEvent& ev) = 0;  // Viewer -> Host
  virtual void OnInput(
      std::function<void(const InputEvent&)> cb) = 0;  // Host 端接收

  // ---- 安全桌面（锁屏/UAC）状态 ----
  // Host 侧：采集线程探测到输入桌面切换时上报，经控制通道送达 Viewer。
  // secure=true 表示宿主已锁屏/弹 UAC；capturable=false 表示宿主进程
  // 打不开安全桌面（未提权），画面与控制中断，解锁后自动恢复。
  virtual void SendSecureDesktopState(bool secure, bool capturable) = 0;
  // Viewer 侧：收到宿主的锁屏状态变化（派发线程触发）。
  virtual void OnSecureDesktop(
      std::function<void(bool secure, bool capturable)> cb) = 0;

  // ---- 事件 / 统计 / 画质 ----
  virtual void OnState(
      std::function<void(State, const std::string& detail)> cb) = 0;
  virtual void OnError(std::function<void(const std::string&)> cb) = 0;
  // Host 端画质档位变化（重配编码器并强制 IDR 后触发）。
  virtual void OnQualityChanged(std::function<void(int level, int w, int h,
                                                   int fps, int kbps)> cb) = 0;
  // Host 端收到 Viewer 每秒解码统计（引擎内部已用于自动降档，此回调仅供观测）。
  virtual void OnRemoteStats(
      std::function<void(int received, int decoded, int decodeFailed)> cb) = 0;

  // 画质模式：0=auto 1=超清 2=高清 3=流畅。Viewer 调用会经控制通道同步到
  // Host（通道打开时立即发送、连接建立后自动补发一次）。
  virtual void SetQualityMode(int mode) = 0;
  virtual Stats GetStats() const = 0;  // 任意线程可调
};

}  // namespace rdc
#endif  // RDC_ENGINE_H_
