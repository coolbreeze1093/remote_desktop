// remote_desktop — background capture thread (wraps screen_capture_lib).
#ifndef REMOTE_DESKTOP_SESSION_SCREEN_CAPTURE_THREAD_H_
#define REMOTE_DESKTOP_SESSION_SCREEN_CAPTURE_THREAD_H_

#include <atomic>
#include <thread>

#include <screen_capture/capture.hpp>

#include "../debug_log.h"

namespace rd {

// Frame consumer interface implemented by the session; callbacks arrive on
// the capture thread. `sink` must outlive the thread (stop() joins first).
class FrameSink {
 public:
  virtual ~FrameSink() = default;
  virtual void onFrame(const scl_frame& raw) = 0;
  virtual void onGeometry(int ox, int oy, int w, int h) = 0;
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

  std::unique_ptr<scl::Capturer> capturer_;
  FrameSink* sink_ = nullptr;
  int intervalMs_ = 50;
  int emptyFrames_ = 0;
  std::atomic_bool running_{false};
  std::thread thread_;
};

}  // namespace rd

#endif
