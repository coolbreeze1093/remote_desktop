/*
 * screen_capture_lib — shared internals.
 */
#include "capture_internal.h"

#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "modules/desktop_capture/desktop_and_cursor_composer.h"

namespace scl_internal {

using webrtc::DesktopCaptureOptions;
using webrtc::DesktopCapturer;
using webrtc::DesktopFrame;

webrtc::DesktopCaptureOptions ToWebrtcOptions(const scl_options* opt) {
  DesktopCaptureOptions o = DesktopCaptureOptions::CreateDefault();
#if defined(RTC_ENABLE_WIN_WGC)
  o.set_allow_wgc_screen_capturer(opt && opt->prefer_wgc != 0);
  o.set_allow_wgc_window_capturer(opt && opt->prefer_wgc != 0);
  o.set_allow_wgc_capturer_fallback(opt && opt->wgc_fallback != 0);
#endif
  o.set_allow_directx_capturer(opt && opt->prefer_directx != 0);
  o.set_disable_effects(opt ? opt->disable_effects != 0 : true);
  o.set_detect_updated_region(opt && opt->detect_updated_region != 0);
  return o;
}

scl_result CopyFrameOut(const webrtc::DesktopFrame& src, scl_frame* out) {
  const int32_t w = src.size().width();
  const int32_t h = src.size().height();
  if (w <= 0 || h <= 0) {
    return SCL_ERR_PERMANENT;
  }
  const size_t packed = static_cast<size_t>(w) * 4;
  const size_t bytes = packed * static_cast<size_t>(h);
  uint8_t* buf = static_cast<uint8_t*>(malloc(bytes));
  if (!buf) {
    return SCL_ERR_OUT_OF_MEMORY;
  }
  const uint8_t* srow = src.data();
  uint8_t* drow = buf;
  for (int32_t y = 0; y < h; ++y) {
    memcpy(drow, srow, packed);
    srow += src.stride();
    drow += packed;
  }
  out->width = w;
  out->height = h;
  out->stride = static_cast<int32_t>(packed);
  out->data = buf;
  out->capture_time_ms = src.capture_time_ms();
  out->top_left_x = src.top_left().x();
  out->top_left_y = src.top_left().y();
  return SCL_OK;
}

scl_result MapResult(DesktopCapturer::Result r) {
  switch (r) {
    case DesktopCapturer::Result::SUCCESS:
      return SCL_OK;
    case DesktopCapturer::Result::ERROR_TEMPORARY:
      return SCL_ERR_TEMPORARY;
    case DesktopCapturer::Result::ERROR_PERMANENT:
      return SCL_ERR_PERMANENT;
  }
  return SCL_ERR_PERMANENT;
}

std::unique_ptr<DesktopCapturer> CreateCapturer(scl_source_type type,
                                                const DesktopCaptureOptions& options,
                                                bool include_cursor) {
  std::unique_ptr<DesktopCapturer> capturer =
      (type == SCL_SOURCE_SCREEN)
          ? DesktopCapturer::CreateScreenCapturer(options)
          : DesktopCapturer::CreateWindowCapturer(options);
  if (!capturer || !include_cursor) {
    return capturer;
  }
  /* The composer owns both the capturer and its MouseCursorMonitor and
   * forwards GetSourceList/SelectSource to the inner capturer. */
  return std::make_unique<webrtc::DesktopAndCursorComposer>(std::move(capturer),
                                                            options);
}

}  // namespace scl_internal
