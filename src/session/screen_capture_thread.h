// remote_desktop — background capture thread (wraps screen_capture_lib).
//
// 锁屏/UAC 支持：输入桌面切到安全桌面（Winlogon）时，DXGI 复制必然失败，
// 只有 GDI 采集器能工作（webrtc 的 ScreenCapturerWinGdi 每帧自切输入桌面）。
// 本线程逐帧探测当前输入桌面，变化时重建采集器（Default → DXGI 优先；
// 安全桌面 → 仅 GDI），并通知 sink 锁屏状态；打不开安全桌面（未提权进程）
// 则暂停采集并如实上报，解锁后自动恢复。
//
// RDP 恢复：当同一用户通过 RDP 连入导致 console session 显示离线时，
// DXGI 持续返回空帧（桌面名仍为 Default，不触发 MaybeSwitchDesktop）。
// 达到连续空帧阈值后强制重建采集器，确保 RDP 断开后采集恢复。
#ifndef REMOTE_DESKTOP_SESSION_SCREEN_CAPTURE_THREAD_H_
#define REMOTE_DESKTOP_SESSION_SCREEN_CAPTURE_THREAD_H_

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <screen_capture/capture.hpp>

#include "../core/cursor_monitor.h"
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
  virtual void onSecureDesktopChanged(bool secureDesktop,
                                      bool capturable) {}
};

// Owns a persistent scl::Capturer and delivers frames at ~fps to a sink.
class ScreenCaptureThread {
 public:
  ScreenCaptureThread() = default;
  ~ScreenCaptureThread() { stop(); }

  // cursor 非空时采集线程每帧顺带轮询一次光标状态（经 sink 的光标回调送
  // 出）；实例归宿主所有（注入线程也会用它做注入后即时轮询），须在 stop()
  // 返回后才可析构。
  bool start(int64_t screenId, int fps, FrameSink* sink,
             CursorMonitor* cursor = nullptr) {
    scl::Options opt;
    opt.prefer_wgc = false;  // DXGI/GDI: suits constant streaming, no WinRT
    // 光标不再烤入视频：位置/形态经控制通道单独送达，主控端本地渲染——
    // 否则光标显示要背上整条编码传输链的延迟，且查看端光标形态永远对不上。
    opt.include_cursor = false;
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
    cursor_ = cursor;
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
    cursor_ = nullptr;
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
          emptyFrames_ = 0;
          rebuildFailures_ = 0;
          forceGdi_ = false;  // success: reset DXGI→GDI fallback
          sink_->onGeometry(static_cast<int>(f.top_left_x),
                            static_cast<int>(f.top_left_y), f.width, f.height);
          scl_frame raw{f.width,   f.height, f.stride,        f.data,
                        f.capture_time_ms, f.top_left_x, f.top_left_y};
          sink_->onFrame(raw);
          // Buffer ownership: scl_frame_free frees what scl_capture allocated.
          scl_frame_free(&raw);
          f.data = nullptr;
        } else if (!f.data) {
          ++emptyFrames_;
          if (emptyFrames_ >= kEmptyFramesRebuild) {
            // Prolonged empty frames without desktop name change
            // (e.g. RDP took over the same-user console session, then
            //  was kicked out by our SendInput — DXGI capturer stuck).
            // Force rebuild on next MaybeSwitchDesktop pass.
            rdDebug("capture",
                    "empty frame x%d/%d — forcing capturer rebuild (last=%s)",
                    emptyFrames_, kEmptyFramesRebuild,
                    scl::ResultString(capturer_->last_result()));
            capturerDesktopName_.clear();
            ++rebuildFailures_;
            if (rebuildFailures_ >= kMaxRebuildFailures && !forceGdi_) {
              forceGdi_ = true;
              rdDebug("capture",
                      "DXGI failed after %d rebuilds — falling back to GDI",
                      kMaxRebuildFailures);
            }
          }
        }
      } catch (const std::exception& e) {
        // A transient capturer failure must not kill the thread.
        // Force rebuild if the capturer broke during session switch (RDP).
        fprintf(stderr, "[capture] exception: %s\n", e.what());
        fflush(stderr);
        rdDebug("capture", "exception: %s", e.what());
        capturerDesktopName_.clear();
        ++rebuildFailures_;
        if (rebuildFailures_ >= kMaxRebuildFailures && !forceGdi_) {
          forceGdi_ = true;
          rdDebug("capture",
                  "DXGI failed after %d rebuilds (exception) — falling back to GDI",
                  kMaxRebuildFailures);
        }
      } catch (...) {
        fprintf(stderr, "[capture] unknown exception\n");
        fflush(stderr);
        rdDebug("capture", "unknown exception");
        capturerDesktopName_.clear();
        ++rebuildFailures_;
        if (rebuildFailures_ >= kMaxRebuildFailures && !forceGdi_) {
          forceGdi_ = true;
          rdDebug("capture",
                  "DXGI failed after %d rebuilds (exception) — falling back to GDI",
                  kMaxRebuildFailures);
        }
      }
      // 光标状态随帧轮询（位置/形态变化才上报；注入线程另有即时轮询）。
      if (cursor_) {
        cursor_->Poll();
      }
      std::this_thread::sleep_until(next);
    }
  }

  // 输入桌面打不开（会话断开中）时的兑底：在当前线程桌面重建 GDI 采集器，
  // 避免 forceGdi 永远不生效。GDI 采集器每帧自切输入桌面（webrtc 行为），
  // 桌面恢复后能自动跟上。
  void RebuildCapturerOnCurrentDesktop() {
    capturer_.reset();
    capturerDesktopName_.clear();
    scl::Options opt;
    opt.prefer_wgc = false;
    opt.prefer_directx = false;  // DXGI 已在持续 temporary error，直接 GDI
    opt.include_cursor = false;
    opt.detect_updated_region = false;
    auto c = scl::Capturer::Create(SCL_SOURCE_SCREEN, opt);
    if (c && c->Select(screenId_)) {
      capturer_ = std::move(c);
      // 视作 Default 占位：探测恢复后 MaybeSwitchDesktop 会按实际桌面再校正。
      capturerDesktopName_ = L"Default";
      rdDebug("capture", "GDI capturer rebuilt on current desktop");
    } else {
      rdDebug("capture", "GDI rebuild FAILED (%s)",
              c ? scl::ResultString(c->last_result()) : "null");
    }
  }

  // 输入桌面变化时重建采集器（失败/未变化时为 no-op，最多每秒重试一次）。
  void MaybeSwitchDesktop() {
    bool denied = false;
    DWORD openErr = 0;
    const std::wstring desktop = rd::CurrentInputDesktopName(&denied, &openErr);
    if (denied) {
      // 打不开当前输入桌面：典型是锁屏/UAC 的安全桌面而本进程未提权。
      // 采集与注入均无权限（旧采集器在锁屏下也只会出空帧），如实上报，
      // 让两端 UI 显示明确原因；解锁后探测恢复，走下方分支补发解除。
      static std::atomic<int> denyDbg{0};
      if (denyDbg.fetch_add(1) < 5) {
        // elevated 标记是关键排查位：=1 仍被拒说明本系统连提权进程也进
        // 不了安全桌面（需转 SYSTEM 服务方案）；=0 说明实例没提权。
        rdDebug("capture",
                "input desktop DENIED (elevated=%d) -> report locked/unprivileged",
                (int)rd::IsProcessElevated());
      }
      NotifySecureDesktop(true, false);
      return;
    }
    if (desktop.empty()) {
      // OpenInputDesktop 失败但非权限拒绝（err≠5）：典型是控制台会话断开中
      // （RDP 接管/无人看控制台）或会话切换瞬间。此前这里静默放弃 → 采集器
      // 永远停留在初始 DXGI 上（虚拟机里 DXGI 常不可用）→ forceGdi 永远不
      // 生效 → 永久空帧（2026-10-26 被控端实测实锤）。改为：落错误码日志，
      // 并在当前线程桌面重建 GDI 采集器继续尝试（限频 1s）；输入桌面恢复
      // 可探测后，正常路径会再按实际桌面接管。
      static std::atomic<int> emptyDbg{0};
      if (emptyDbg.fetch_add(1) < 5) {
        rdDebug("capture",
                "OpenInputDesktop failed err=%lu — rebuild GDI on current desktop",
                (unsigned long)openErr);
      }
      const auto now = std::chrono::steady_clock::now();
      if (now - lastDesktopAttempt_ < std::chrono::seconds(1)) {
        return;  // 重建退避
      }
      lastDesktopAttempt_ = now;
      RebuildCapturerOnCurrentDesktop();
      return;
    }
    if (desktop == capturerDesktopName_) {
      // 桌面未变；若此前上报过"未提权锁屏不可控"（denied 分支），此刻
      // 已解锁，补发一条解除。稳态下去重后是无操作。
      NotifySecureDesktop(false, true);
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - lastDesktopAttempt_ < std::chrono::seconds(1)) {
      return;  // 重建失败退避
    }
    lastDesktopAttempt_ = now;
    SwitchCapturerToDesktop(desktop);
  }

  // 锁屏状态上报（按状态去重，避免 UI/网络每帧被刷）。
  void NotifySecureDesktop(bool secure, bool capturable) {
    if (sink_ && (secure != reportedSecure_ || capturable != reportedCapturable_)) {
      reportedSecure_ = secure;
      reportedCapturable_ = capturable;
      sink_->onSecureDesktopChanged(secure, capturable);
    }
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
      // 安全桌面只能 GDI（DXGI 复制在锁屏下被拒）；平时 DXGI 优先，
      // 但 DXGI 连续重建失败后（如 RDP 切换后显示未恢复）降级到 GDI。
      scl::Options opt;
      opt.prefer_wgc = false;
      opt.prefer_directx = !secure && !forceGdi_;
      opt.include_cursor = false;
      opt.detect_updated_region = false;
      auto c = scl::Capturer::Create(SCL_SOURCE_SCREEN, opt);
      if (c && c->Select(screenId_)) {
        capturer_ = std::move(c);
        capturerDesktopName_ = desktop;
        rdDebug("capture", "capturer ready for desktop '%s' (dxgi=%d gdiFallback=%d)",
                ToAscii(desktop).c_str(), !secure && !forceGdi_, (int)forceGdi_);
      } else {
        rdDebug("capture", "recreate capturer FAILED for '%s' (%s)",
                ToAscii(desktop).c_str(),
                c ? scl::ResultString(c->last_result()) : "null");
      }
    }

    emptyFrames_ = 0;
    NotifySecureDesktop(secure, capturable);
  }

  static std::string ToAscii(const std::wstring& w) {
    return {w.begin(), w.end()};  // 桌面名（Default/Winlogon）都是 ASCII
  }

  std::unique_ptr<scl::Capturer> capturer_;
  FrameSink* sink_ = nullptr;
  CursorMonitor* cursor_ = nullptr;  // 宿主所有；仅采集线程 Poll
  int intervalMs_ = 50;
  int emptyFrames_ = 0;
  // After this many consecutive empty frames, force capturer rebuild
  // even if the desktop name hasn't changed (RDP session-switch recovery).
  // NOTE: Capture(1000) blocks 1s per empty frame. Set to 1 for fastest
  // recovery; the 1s rate-limit in MaybeSwitchDesktop prevents thrashing.
  static constexpr int kEmptyFramesRebuild = 1;
  // After this many consecutive rebuild failures, switch from DXGI to GDI
  // (DXGI may be permanently broken after RDP session switch on same account).
  static constexpr int kMaxRebuildFailures = 5;
  // Tracks consecutive rebuild attempts that still produced empty frames.
  int rebuildFailures_ = 0;
  // When true, SwitchCapturerToDesktop forces GDI even on Default desktop.
  bool forceGdi_ = false;
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