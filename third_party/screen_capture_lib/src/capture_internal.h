/*
 * screen_capture_lib — internal helpers shared by the C API and the C++
 * facade. Not installed, not part of the public surface.
 */
#ifndef SCREEN_CAPTURE_SRC_CAPTURE_INTERNAL_H_
#define SCREEN_CAPTURE_SRC_CAPTURE_INTERNAL_H_

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "screen_capture/screen_capture.h"

namespace scl_internal {

/* Maps the public option struct onto WebRTC's DesktopCaptureOptions. */
webrtc::DesktopCaptureOptions ToWebrtcOptions(const scl_options* opt);

/* Callback adapter that turns one CaptureFrame() call into a blocking,
 * timeout-bounded wait. */
class BlockingCallback : public webrtc::DesktopCapturer::Callback {
 public:
  void OnFrameCaptureStart() override {}

  void OnCaptureResult(webrtc::DesktopCapturer::Result result,
                       std::unique_ptr<webrtc::DesktopFrame> frame) override {
    std::lock_guard<std::mutex> lock(mutex_);
    result_ = result;
    frame_ = std::move(frame);
    done_ = true;
    cv_.notify_all();
  }

  /* Returns true when a result arrived before the timeout. */
  bool Wait(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [this] { return done_; });
  }

  /* Clears any stale result so a Capture() call cannot pick up the frame of
   * a previous attempt (e.g. a callback that arrived after its timeout). */
  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    done_ = false;
    frame_.reset();
  }

  webrtc::DesktopCapturer::Result result() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return result_;
  }

  std::unique_ptr<webrtc::DesktopFrame> TakeFrame() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::move(frame_);
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool done_ = false;
  webrtc::DesktopCapturer::Result result_ =
      webrtc::DesktopCapturer::Result::ERROR_TEMPORARY;
  std::unique_ptr<webrtc::DesktopFrame> frame_;
};

/* Packs a DesktopFrame (arbitrary stride) into a tightly-packed BGRA buffer
 * allocated with malloc; released via scl_frame_free/scl_free_buffer. */
scl_result CopyFrameOut(const webrtc::DesktopFrame& src, scl_frame* out);

scl_result MapResult(webrtc::DesktopCapturer::Result r);

/* Creates the platform capturer for `type`, optionally wrapped in a
 * DesktopAndCursorComposer that composites the mouse cursor into frames. */
std::unique_ptr<webrtc::DesktopCapturer> CreateCapturer(
    scl_source_type type,
    const webrtc::DesktopCaptureOptions& options,
    bool include_cursor);

}  // namespace scl_internal

#endif /* SCREEN_CAPTURE_SRC_CAPTURE_INTERNAL_H_ */
