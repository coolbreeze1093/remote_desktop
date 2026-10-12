// remote_desktop — background capture thread (wraps screen_capture_lib).
//
// 锁屏/UAC 支持：输入桌面切到安全桌面（Winlogon）时，DXGI 复制必然失败，
// 只有 GDI 采集器能工作（webrtc 的 ScreenCapturerWinGdi 每帧自切输入桌面）。
// 本线程逐帧探测当前输入桌面，变化时重建采集器（Default → DXGI 优先；
// 安全桌面 → 仅 GDI），并通知 sink 锁屏状态；打不开安全桌面（未提权进程）
// 则暂停采集并如实上报，解锁后自动恢复。
#ifndef REMOTE_DESKTOP_SESSION_SCREEN_CAPTURE_THREAD_H_
#define REMOTE_DESKTOP_SESSION_SCREEN_CAPTURE_THREAD_H_

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <screen_capture/capture.hpp>

#include "../core/input_desktop.h"
#include "../debug_log.h"

namespace rd {

// Frame consumer interface implemented by the session; callbacks arrive on
// the capture thread. `sink` must outlive the thread (stop() joins first).
class FrameSink {
 public:
  virtual ~FrameSink() = default;
  virtual void onFrame(const scl_frame& raw) = 0;
  virtual void onGeometry(int ox, int oy, int w, int h) = 0;
  // 安全桌面（锁屏/UAC）状态变化；采集线程触发。secureDesktop=true 表示
  // 输入桌面已不是 Default；capturable=false 表示本进程打不开该桌面（未
  // 提权），锁屏画面无法采集。默认空实现：老 sink 不受影响。
  virtual void onSecureDesktopChanged(bool /*secureDesktop*/,
                                      bool /*capturable*/) {}
};

// Owns a persistent scl::Capturer and delivers frames at ~fps to a sink.
class ScreenCaptureThread {
 public:
  ScreenCaptureThread() = default;
  ~ScreenCaptureThread() { stop(); }

  bool start(int64_t screenId, int fps, FrameSink* sink) {
    scl::Options opt;
    opt.prefer_wgc = false;  // DXGI/GDI: suits constant streaming, no WinRT
    opt.include_cursor = true;
    opt.detect_updated_region = false;
    capturer_ = scl::Capturer::Create(SCL_SOURCE_SCREEN, opt);
    if (!capturer_ || !capturer_->Select(screenId)) {
      rdDebug("capture", "capturer create/select FAILED (screen=%lld, res=%s)",
              (long long)screenId,
              capturer_ ? scl::ResultString(capturer_->last_result()) : "null");
      capturer_.reset();
      return false;
    }
    rdDebug("capture", "capturer ready (screen=%lld, %dfps)",
            (long long)screenId, fps);
    screenId_ = screenId;
    capturerDesktopName_ = L"Default";
    sink_ = sink;
    intervalMs_ = 1000 / (fps > 0 ? fps : 20);
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
  }

  void stop() {
    running_ = false;
    if (thread_.joinable()) {
      thread_.join();
    }
    capturer_.reset();
    sink_ = nullptr;
  }

 private:
  void run() {
    while (running_) {
      const auto next = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(intervalMs_);
      try {
        MaybeSwitchDesktop();
        if (!capturer_) {
          // 打不开安全桌面（未提权）时暂停采集：与其推黑帧，不如冻结最后
          // 一帧并让 UI 明确提示，解锁后自动恢复。
          std::this_thread::sleep_until(next);
          continue;
        }
        scl::Frame f = capturer_->Capture(1000);
        if (f.data && sink_) {
          sink_->onGeometry(static_cast<int>(f.top_left_x),
                            static_cast<int>(f.top_left_y), f.width, f.height);
          scl_frame raw{f.width,   f.height, f.stride,        f.data,
                        f.capture_time_ms, f.top_left_x, f.top_left_y};
          sink_->onFrame(raw);
          // Buffer ownership: scl_frame_free frees what scl_capture allocated.
          scl_frame_free(&raw);
          f.data = nullptr;
        } else if (!f.data && ++emptyFrames_ % (5000 / intervalMs_) == 1) {
          // Persistent empty frames (transient DXGI failures): surface them
          // roughly once per 5s, this is how a broken capture looks.
          rdDebug("capture", "empty frame x%d (last=%s)", emptyFrames_,
                  scl::ResultString(capturer_->last_result()));
        }
      } catch (const std::exception& e) {
        // A transient capturer failure must not kill the thread.
        fprintf(stderr, "[capture] exception: %s\n", e.what());
        fflush(stderr);
        rdDebug("capture", "exception: %s", e.what());
      } catch (...) {
        fprintf(stderr, "[capture] unknown exception\n");
        fflush(stderr);
        rdDebug("capture", "unknown exception");
      }
      std::this_thread::sleep_until(next);
    }
  }

  // 输入桌面变化时重建采集器（失败/未变化时为 no-op，最多每秒重试一次）。
  void MaybeSwitchDesktop() {
    const std::wstring desktop = rd::CurrentInputDesktopName();
    if (desktop.empty() || desktop == capturerDesktopName_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - lastDesktopAttempt_ < std::chrono::seconds(1)) {
      return;  // 重建失败退避
    }
    lastDesktopAttempt_ = now;
    SwitchCapturerToDesktop(desktop);
  }

  void SwitchCapturerToDesktop(const std::wstring& desktop) {
    const bool secure = rd::IsSecureDesktopName(desktop);
    // 先释放采集器（GDI DC 随析构释放），线程才能 SetThreadDesktop 切换。
    capturer_.reset();
    capturerDesktopName_.clear();

    std::wstring attached;
    const rd::AttachResult att = rd::AttachThisThreadToInputDesktop(&attached);
    // kProbeFailed：桌面句柄拿到了但名字读不出来——放行，GDI 采集器每帧
    // 自切逻辑仍能兜底；其余失败（典型是未提权时 kOpenDenied）不可采集。
    const bool capturable = att == rd::AttachResult::kOnInputDesktop ||
                            att == rd::AttachResult::kProbeFailed;
    if (att != rd::AttachResult::kOnInputDesktop) {
      static std::atomic<int> attDbg{0};
      if (attDbg.fetch_add(1) < 10) {
        rdDebug("capture", "attach to input desktop failed (%d)", (int)att);
      }
    }

    if (capturable) {
      // 安全桌面只能 GDI（DXGI 复制在锁屏下被拒）；平时 DXGI 优先。
      scl::Options opt;
      opt.prefer_wgc = false;
      opt.prefer_directx = !secure;
      opt.include_cursor = true;
      opt.detect_updated_region = false;
      auto c = scl::Capturer::Create(SCL_SOURCE_SCREEN, opt);
      if (c && c->Select(screenId_)) {
        capturer_ = std::move(c);
        capturerDesktopName_ = desktop;
        rdDebug("capture", "capturer ready for desktop '%s' (dxgi=%d)",
                ToAscii(desktop).c_str(), !secure);
      } else {
        rdDebug("capture", "recreate capturer FAILED for '%s' (%s)",
                ToAscii(desktop).c_str(),
                c ? scl::ResultString(c->last_result()) : "null");
      }
    }

    emptyFrames_ = 0;
    if (sink_ && (secure != reportedSecure_ || capturable != reportedCapturable_)) {
      reportedSecure_ = secure;
      reportedCapturable_ = capturable;
      sink_->onSecureDesktopChanged(secure, capturable);
    }
  }

  static std::string ToAscii(const std::wstring& w) {
    return {w.begin(), w.end()};  // 桌面名（Default/Winlogon）都是 ASCII
  }

  std::unique_ptr<scl::Capturer> capturer_;
  FrameSink* sink_ = nullptr;
  int intervalMs_ = 50;
  int emptyFrames_ = 0;
  int64_t screenId_ = 0;
  // 当前采集器对应的输入桌面；空串表示"上次重建未成功"，触发重试。
  std::wstring capturerDesktopName_ = L"Default";
  std::chrono::steady_clock::time_point lastDesktopAttempt_ =
      std::chrono::steady_clock::time_point::min();
  // 已上报给 sink 的锁屏状态（去重，避免 UI/网络每秒被刷）。
  bool reportedSecure_ = false;
  bool reportedCapturable_ = true;
  std::atomic_bool running_{false};
  std::thread thread_;
};

}  // namespace rd

#endif
