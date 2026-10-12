// remote_desktop — Qt 侧薄适配器：把 rdc 引擎的回调/接口映射为 Qt 信号。
//
// 本类曾经身兼采集/编码/解码/传输/画质全流程；引擎（rdc）落地后只剩：
//   - 持有 rdc::Engine 并接线回调（状态/错误 → Qt 信号）
//   - 采集线程（ScreenCaptureThread）把帧推进引擎
//   - Host 端收输入事件 → InputInjector 注入（Win32 留在宿主层）
//   - Viewer 端收解码帧 → I420→QImage → frameReady 信号
//   - SDP 阻塞调用期间用 QEventLoop 泵事件，保持 UI 响应
#ifndef REMOTE_DESKTOP_SESSION_REMOTE_DESKTOP_SESSION_H_
#define REMOTE_DESKTOP_SESSION_REMOTE_DESKTOP_SESSION_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include <QImage>
#include <QObject>
#include <QString>

#include "core/input_event.h"
#include "core/input_injector.h"
#include "session/screen_capture_thread.h"

namespace rdc {
class Engine;
}

namespace rd {

struct SelfTestHooks;  // test-only backdoor, defined in self_test.cc

class RemoteDesktopSession : public QObject {
  Q_OBJECT
 public:
  enum class Role { None, Host, Viewer };

  // 管道计数器（UI 轮询用，口径与引擎 Stats 一致：捕获→编码→发送→
  // 接收→解码→显示）。
  struct DiagStats {
    uint32_t captured = 0;
    uint32_t encoded = 0;
    uint32_t sent = 0;
    uint32_t dropped = 0;
    uint32_t received = 0;
    uint32_t decoded = 0;
    uint32_t decodeFailed = 0;
    int qualityLevel = -1;
    int encodeWidth = 0;
    int encodeHeight = 0;
    int encodeMs = 0;
  };

  explicit RemoteDesktopSession(QObject* parent = nullptr);
  ~RemoteDesktopSession() override;

  // --- Host（被控端）---
  bool startHost(int64_t screenId);
  QString hostAcceptOffer(const QString& offerSdp);  // 返回 answer SDP
  // UI 便捷封装：startHost + 应用 offer；任何失败都会 stop() 便于重试。
  QString hostAcceptOfferChecked(const QString& offerSdp, int64_t screenId);

  // --- Viewer（主控端）---
  bool startViewer();
  QString viewerCreateOffer();  // 返回 offer SDP
  bool viewerAcceptAnswer(const QString& answerSdp);

  void stop();

  Role role() const { return role_; }
  // 画质模式：0=auto 1=超清 2=高清 3=流畅。Viewer 调用会经控制通道同步。
  void setVideoQualityMode(int mode);
  void sendInput(const rd::InputEvent& ev);
  DiagStats diagStats() const;

 signals:
  void connected();
  void disconnected();
  void failed(const QString& reason);
  void statusText(const QString& text);
  void frameReady(const QImage& image);
  void inputInjected();
  // Host：本机输入桌面切到/离开安全桌面（锁屏/UAC）。capturable=false
  // 表示本进程打不开安全桌面（未提权），画面/控制中断，解锁后自动恢复。
  // 采集线程触发；同时经控制通道送达 Viewer（peerSecureDesktopChanged）。
  void hostSecureDesktopChanged(bool secure, bool capturable);
  // Viewer：收到宿主的锁屏状态变化（引擎派发线程触发）。
  void peerSecureDesktopChanged(bool secure, bool capturable);

 private:
  friend struct SelfTestHooks;
  bool probeScreen(int64_t screenId);
  void wireEngineCallbacks();
  QString runBlockingPumped(const std::function<std::string()>& fn);
  // 采集线程的锁屏状态变化入口：本机 GUI 提示 + 经控制通道送达 Viewer。
  // 供 CaptureSink（采集线程）与 self_test 钩子调用，引擎 Send 线程安全。
  void HandleSecureDesktopChange(bool secure, bool capturable);

  class CaptureSink;

  Role role_ = Role::None;
  // 每次 startHost/startViewer 重建：stop() 翻转旧守卫后，新会话要拿新守卫，
  // 否则重启后所有回调都会被旧标志拦掉。
  std::shared_ptr<std::atomic_bool> alive_ =
      std::make_shared<std::atomic_bool>(true);
  std::shared_ptr<rdc::Engine> engine_;
  std::unique_ptr<ScreenCaptureThread> capture_;
  std::unique_ptr<FrameSink> sink_;
  InputInjector injector_;
};

}  // namespace rd

#endif  // REMOTE_DESKTOP_SESSION_REMOTE_DESKTOP_SESSION_H_
