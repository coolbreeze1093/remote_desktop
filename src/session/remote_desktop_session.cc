#include "session/remote_desktop_session.h"

#include <QEventLoop>
#include <QMetaObject>

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>

#include <screen_capture/capture.hpp>

#include "debug_log.h"
#include "rdc/engine.h"

namespace rd {

namespace {
constexpr int kCaptureFps = 24;  // 采集/节拍频率（引擎每 24 帧评估一次画质）
}  // namespace

// 采集槽：采集线程回调 → 推帧进引擎 / 更新注入器几何。
// onFrame 在采集线程触发；PushVideoFrameBgra 同步拷贝完才返回，因此调用方
// 随后的 scl_frame_free 安全（与旧 HostLoopSink 的 memcpy 语义一致）。
class RemoteDesktopSession::CaptureSink final : public FrameSink {
 public:
  CaptureSink(RemoteDesktopSession* s,
              std::shared_ptr<std::atomic_bool> alive)
      : s_(s), alive_(std::move(alive)) {}

  void onFrame(const scl_frame& raw) override {
    if (!alive_->load(std::memory_order_acquire) || !s_->engine_) {
      return;
    }
    s_->engine_->PushVideoFrameBgra(raw.data, raw.stride, raw.width,
                                    raw.height);
  }

  void onGeometry(int ox, int oy, int w, int h) override {
    if (!alive_->load(std::memory_order_acquire)) {
      return;
    }
    // InputInjector / CursorMonitor 字段是原子的，采集线程直接更新即可。
    s_->injector_.setFrameOrigin(ox, oy);
    s_->injector_.setFrameSize(w, h);
    s_->cursor_.setFrameGeometry(ox, oy, w, h);
    static std::atomic<int> dbg{0};
    if (dbg.fetch_add(1) < 3) {
      rdDebug("session", "geometry origin=(%d,%d) size=%dx%d", ox, oy, w, h);
    }
  }

  // 锁屏/UAC 状态变化（采集线程触发）：本机 GUI 提示 + 送达 Viewer。
  void onSecureDesktopChanged(bool secureDesktop, bool capturable) override {
    if (!alive_->load(std::memory_order_acquire)) {
      return;
    }
    rdDebug("session", "secure desktop changed secure=%d capturable=%d",
            (int)secureDesktop, (int)capturable);
    s_->HandleSecureDesktopChange(secureDesktop, capturable);
  }

 private:
  RemoteDesktopSession* s_;
  std::shared_ptr<std::atomic_bool> alive_;
};

RemoteDesktopSession::RemoteDesktopSession(QObject* parent) : QObject(parent) {}

RemoteDesktopSession::~RemoteDesktopSession() { stop(); }

bool RemoteDesktopSession::probeScreen(int64_t screenId) {
  // 有效性探测：能否为该屏创建采集器并选中。
  scl::Options opt;
  auto test = scl::Capturer::Create(SCL_SOURCE_SCREEN, opt);
  if (!test) {
    return false;
  }
  return test->Select(screenId);
}

void RemoteDesktopSession::wireEngineCallbacks() {
  const auto alive = alive_;

  engine_->OnState([this, alive](rdc::State s, const std::string& detail) {
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    switch (s) {
      case rdc::State::Connected:
        rdDebug("session", "connected (role=%s)",
                role_ == Role::Host ? "host" : "viewer");
        if (role_ == Role::Host) {
          // 主控端刚连上：补发当前光标位置/形态，否则要等下一次变化才能
          // 看到光标。
          cursor_.Poll(true);
        }
        emit connected();
        break;
      case rdc::State::Closed:
        emit disconnected();
        break;
      case rdc::State::Failed:
        emit failed(QString::fromStdString(
            detail.empty() ? QStringLiteral("连接失败").toStdString() : detail));
        break;
      default:
        emit statusText(QString::fromStdString(detail));
        break;
    }
  });

  engine_->OnError([this, alive](const std::string& error) {
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    emit failed(QString::fromStdString(error));
  });

  // Host：远端输入事件 → Win32 注入（引擎派发线程触发；injector 原子安全，
  // queued 信号跨线程投递）。
  engine_->OnInput([this, alive](const rdc::InputEvent& ev) {
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    static std::atomic<int> dbg{0};
    if (dbg.fetch_add(1) < 10) {
      rdDebug("session", "inputReceived role=%s type=%d",
              role_ == Role::Host ? "host" : "other", static_cast<int>(ev.type));
    }
    if (role_ != Role::Host) {
      return;
    }
    injector_.inject(ev);
    if (ev.type == InputEvent::Type::MouseMove ||
        ev.type == InputEvent::Type::MouseButton) {
      // 注入后立即回报光标位置：主控端本地光标不等下一个采集帧就能跟上。
      cursor_.Poll();
    }
    emit inputInjected();
  });

  // Viewer：宿主光标位置/形态 → 信号（引擎派发线程触发）。
  engine_->OnCursorPosition(
      [this, alive](double nx, double ny, bool visible) {
        if (!alive->load(std::memory_order_acquire)) {
          return;
        }
        emit cursorMoved(nx, ny, visible);
      });
  engine_->OnCursorShape(
      [this, alive](std::shared_ptr<const rdc::CursorShape> shape) {
        if (!alive->load(std::memory_order_acquire) || !shape) {
          return;
        }
        QImage img(static_cast<int>(shape->width),
                   static_cast<int>(shape->height),
                   QImage::Format_ARGB32_Premultiplied);
        if (shape->bgra.size() != static_cast<size_t>(img.sizeInBytes())) {
          return;
        }
        std::memcpy(img.bits(), shape->bgra.data(), shape->bgra.size());
        emit cursorShapeChanged(img, static_cast<int>(shape->hotspotX),
                                static_cast<int>(shape->hotspotY));
      });

  // Viewer：解码帧 → QImage → frameReady（引擎解码线程触发）。
  engine_->OnVideoFrame(
      [this, alive](std::shared_ptr<const rdc::VideoFrame> frame) {
        if (!alive->load(std::memory_order_acquire) || !frame) {
          return;
        }
        QImage img(frame->width, frame->height, QImage::Format_RGB32);
        rdc::I420ToBgra(frame->y, frame->yStride, frame->u, frame->uStride,
                        frame->v, frame->vStride, frame->width, frame->height,
                        img.bits(), img.bytesPerLine());
        // RD_DUMP_FIRST_FRAME=<png 路径>：把首帧解码结果存图（端到端自检）。
        static std::atomic<bool> dumped{false};
        if (!dumped.exchange(true)) {
          const QString dumpPath = qEnvironmentVariable("RD_DUMP_FIRST_FRAME");
          if (!dumpPath.isEmpty()) {
            img.copy().save(dumpPath, "PNG");
            rdDebug("session", "dumped first decoded frame %dx%d -> %s",
                    img.width(), img.height(),
                    dumpPath.toUtf8().constData());
          }
        }
        emit frameReady(img.copy());
      });

  // Viewer：宿主的锁屏/UAC 状态变化（引擎派发线程触发）。
  engine_->OnSecureDesktop([this, alive](bool secure, bool capturable) {
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    emit peerSecureDesktopChanged(secure, capturable);
  });
}

void RemoteDesktopSession::HandleSecureDesktopChange(bool secure,
                                                     bool capturable) {
  // Qt5 信号是 public：采集线程可直接 emit（auto 连接排队到 UI 线程）。
  emit hostSecureDesktopChanged(secure, capturable);
  if (engine_) {
    engine_->SendSecureDesktopState(secure, capturable);
  }
}

bool RemoteDesktopSession::startHost(int64_t screenId) {
  if (role_ != Role::None || !probeScreen(screenId)) {
    return false;
  }
  role_ = Role::Host;
  alive_ = std::make_shared<std::atomic_bool>(true);
  rdc::Config config;
  config.iceServers = {"stun.miwifi.com:3478"};
  engine_ = std::shared_ptr<rdc::Engine>(rdc::Engine::Create(rdc::Role::Host, config));
  wireEngineCallbacks();
  // 光标轮询 → 控制通道（采集线程与注入线程都会触发；alive_ 兜底防止
  // stop 后误触引擎）。
  const auto alive = alive_;
  cursor_.setCallbacks(
      [this, alive](double nx, double ny, bool visible) {
        if (alive->load(std::memory_order_acquire) && engine_) {
          engine_->SendCursorPosition(nx, ny, visible);
        }
      },
      [this, alive](const CursorShapeData& shape) {
        if (!alive->load(std::memory_order_acquire) || !engine_) {
          return;
        }
        rdc::CursorShape cs;
        cs.width = static_cast<uint32_t>(shape.width);
        cs.height = static_cast<uint32_t>(shape.height);
        cs.hotspotX = static_cast<uint32_t>(shape.hotspotX);
        cs.hotspotY = static_cast<uint32_t>(shape.hotspotY);
        cs.bgra = shape.bgra;
        engine_->SendCursorShape(cs);
      });
  if (!engine_->Start()) {
    stop();
    emit failed(QStringLiteral("编码器初始化失败"));
    return false;
  }
  sink_ = std::make_unique<CaptureSink>(this, alive_);
  capture_ = std::make_unique<ScreenCaptureThread>();
  if (!capture_->start(screenId, kCaptureFps, sink_.get(), &cursor_)) {
    rdDebug("session", "capture start FAILED (screen=%lld)",
            (long long)screenId);
    stop();
    emit failed(QStringLiteral("屏幕捕获启动失败"));
    return false;
  }
  rdDebug("session", "host started (screen=%lld, %dfps)",
          (long long)screenId, kCaptureFps);
  return true;
}

QString RemoteDesktopSession::hostAcceptOffer(const QString& offerSdp) {
  if (role_ != Role::Host || !engine_) {
    return {};
  }
  const std::string offer = offerSdp.toStdString();
  return runBlockingPumped([this, offer] {
    return engine_->AcceptOfferCreateAnswer(offer);
  });
}

QString RemoteDesktopSession::hostAcceptOfferChecked(const QString& offerSdp,
                                                     int64_t screenId) {
  if (!startHost(screenId)) {
    return {};
  }
  const QString answer = hostAcceptOffer(offerSdp);
  if (answer.isEmpty()) {
    stop();
    return {};
  }
  return answer;
}

bool RemoteDesktopSession::startViewer() {
  if (role_ != Role::None) {
    return false;
  }
  role_ = Role::Viewer;
  alive_ = std::make_shared<std::atomic_bool>(true);
  engine_ =
      std::shared_ptr<rdc::Engine>(rdc::Engine::Create(rdc::Role::Viewer));
  wireEngineCallbacks();
  if (!engine_->Start()) {
    stop();
    emit failed(QStringLiteral("解码器初始化失败"));
    return false;
  }
  rdDebug("session", "viewer started");
  return true;
}

QString RemoteDesktopSession::viewerCreateOffer() {
  if (role_ != Role::Viewer || !engine_) {
    return {};
  }
  return runBlockingPumped([this] { return engine_->CreateOffer(); });
}

bool RemoteDesktopSession::viewerAcceptAnswer(const QString& answerSdp) {
  if (role_ != Role::Viewer || !engine_) {
    return false;
  }
  return engine_->AcceptAnswer(answerSdp.toStdString());
}

void RemoteDesktopSession::stop() {
  // 顺序：先停采集（不再推帧）→ 关引擎（内部先断 DC/PC 再 join 线程）。
  alive_->store(false, std::memory_order_release);
  if (capture_) {
    capture_->stop();
  }
  capture_.reset();
  sink_.reset();
  if (engine_) {
    engine_->Close();
    engine_.reset();
  }
  role_ = Role::None;
}

void RemoteDesktopSession::setVideoQualityMode(int mode) {
  if (!engine_) {
    return;
  }
  rdDebug("quality", "mode set to %d (role=%s)", mode,
          role_ == Role::Host ? "host" : "viewer");
  engine_->SetQualityMode(mode);
}

void RemoteDesktopSession::sendInput(const rd::InputEvent& ev) {
  if (engine_) {
    engine_->SendInput(ev);
  }
}

RemoteDesktopSession::DiagStats RemoteDesktopSession::diagStats() const {
  const rdc::Stats st =
      engine_ ? engine_->GetStats() : rdc::Stats{};
  DiagStats out;
  out.captured = st.captured;
  out.encoded = st.encoded;
  out.sent = st.sent;
  out.dropped = st.dropped;
  out.received = st.received;
  out.decoded = st.decoded;
  out.decodeFailed = st.decodeFailed;
  out.qualityLevel = st.qualityLevel;
  out.encodeWidth = st.encodeWidth;
  out.encodeHeight = st.encodeHeight;
  out.encodeMs = st.encodeMs;
  return out;
}

QString RemoteDesktopSession::runBlockingPumped(
    const std::function<std::string()>& fn) {
  // SDP 操作会阻塞等 ICE 收集（上限 10s）：工作线程跑引擎调用（纯 std::string，
  // 不碰任何 Qt 对象——mingw 版 Qt 在非 Qt 线程上构造/析构 QString 会走
  // TLS 析构链，曾有堆损坏），本线程用 QEventLoop 泵事件，UI 保持响应。
  // invokeMethod 是官方支持的跨线程唤醒方式。
  QEventLoop loop;
  std::string result;
  std::thread worker([&loop, &result, fn] {
    result = fn();
    QMetaObject::invokeMethod(&loop, &QEventLoop::quit,
                              Qt::QueuedConnection);
  });
  loop.exec();
  worker.join();
  return QString::fromStdString(result);
}

}  // namespace rd
