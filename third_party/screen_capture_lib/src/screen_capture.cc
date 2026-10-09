/*
 * screen_capture_lib — implementation of the C API.
 *
 * Wraps WebRTC's modules/desktop_capture. Every public call is self-contained
 * and exception-safe: no exception crosses the C boundary.
 */
#include "screen_capture/screen_capture.h"

#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "capture_internal.h"

namespace {

constexpr int kDefaultTimeoutMs = 2000;

/* The C API hands out `const char*` titles, so the strings must outlive the
 * call. They are parked in this cache and remain valid until the next
 * enumeration of the same source type. */
std::mutex g_title_mutex;
std::array<std::vector<std::string>, 2> g_title_cache;

}  // namespace

/* ------------------------------------------------------------- version */

const char* scl_version_string(void) {
  return "screen_capture_lib 1.0.0 (WebRTC desktop_capture)";
}

const char* scl_result_string(scl_result r) {
  switch (r) {
    case SCL_OK:
      return "ok";
    case SCL_ERR_INVALID_ARG:
      return "invalid argument";
    case SCL_ERR_NOT_FOUND:
      return "source not found";
    case SCL_ERR_TEMPORARY:
      return "temporary error, retry";
    case SCL_ERR_PERMANENT:
      return "permanent error";
    case SCL_ERR_UNSUPPORTED:
      return "unsupported";
    case SCL_ERR_OUT_OF_MEMORY:
      return "out of memory";
  }
  return "unknown";
}

/* --------------------------------------------------------------- options */

scl_result scl_default_options(scl_options* out) {
  if (!out) {
    return SCL_ERR_INVALID_ARG;
  }
  out->prefer_wgc = 1;
  out->wgc_fallback = 1;
  out->prefer_directx = 1;
  out->disable_effects = 1;
  out->include_cursor = 1;
  out->detect_updated_region = 0;
  out->capture_timeout_ms = kDefaultTimeoutMs;
  return SCL_OK;
}

/* ---------------------------------------------------------- enumeration */

scl_result scl_enum_sources(scl_source_type type,
                            const scl_options* opt,
                            scl_source_info* buf,
                            int32_t* count) {
  if (!count) {
    return SCL_ERR_INVALID_ARG;
  }
  if (type != SCL_SOURCE_SCREEN && type != SCL_SOURCE_WINDOW) {
    return SCL_ERR_INVALID_ARG;
  }
  const webrtc::DesktopCaptureOptions options =
      scl_internal::ToWebrtcOptions(opt);

  std::unique_ptr<webrtc::DesktopCapturer> capturer =
      scl_internal::CreateCapturer(type, options, /*include_cursor=*/false);
  if (!capturer) {
    return SCL_ERR_UNSUPPORTED;
  }

  webrtc::DesktopCapturer::SourceList sources;
  if (!capturer->GetSourceList(&sources)) {
    return SCL_ERR_PERMANENT;
  }

  if (!buf) {
    *count = static_cast<int32_t>(sources.size());
    return SCL_OK;
  }
  const int32_t n =
      std::min<int32_t>(*count, static_cast<int32_t>(sources.size()));
  {
    std::lock_guard<std::mutex> lock(g_title_mutex);
    std::vector<std::string>& cache = g_title_cache[static_cast<int>(type)];
    cache.clear();
    cache.reserve(n);
    for (int32_t i = 0; i < n; ++i) {
      cache.push_back(sources[i].title);
      buf[i].id = static_cast<int64_t>(sources[i].id);
      buf[i].title = cache.back().c_str();
    }
  }
  *count = n;
  return SCL_OK;
}

/* -------------------------------------------------------------- capture */

scl_result scl_capture(scl_source_type type,
                       int64_t source_id,
                       const scl_options* opt,
                       scl_frame* out) {
  if (!out) {
    return SCL_ERR_INVALID_ARG;
  }
  std::memset(out, 0, sizeof(*out));
  if (type != SCL_SOURCE_SCREEN && type != SCL_SOURCE_WINDOW) {
    return SCL_ERR_INVALID_ARG;
  }

  const webrtc::DesktopCaptureOptions options =
      scl_internal::ToWebrtcOptions(opt);
  const int timeout_ms =
      (opt && opt->capture_timeout_ms > 0) ? opt->capture_timeout_ms
                                           : kDefaultTimeoutMs;

  std::unique_ptr<webrtc::DesktopCapturer> capturer =
      scl_internal::CreateCapturer(type, options, /*include_cursor=*/true);
  if (!capturer) {
    return SCL_ERR_UNSUPPORTED;
  }

  scl_internal::BlockingCallback cb;
  capturer->Start(&cb);

  if (!capturer->SelectSource(
          static_cast<webrtc::DesktopCapturer::SourceId>(source_id))) {
    return SCL_ERR_NOT_FOUND;
  }

  capturer->CaptureFrame();
  if (!cb.Wait(timeout_ms)) {
    return SCL_ERR_TEMPORARY;
  }
  const scl_result mapped = scl_internal::MapResult(cb.result());
  if (mapped != SCL_OK) {
    return mapped;
  }
  std::unique_ptr<webrtc::DesktopFrame> frame = cb.TakeFrame();
  if (!frame) {
    return SCL_ERR_PERMANENT;
  }
  return scl_internal::CopyFrameOut(*frame, out);
}

void scl_frame_free(scl_frame* frame) {
  if (frame && frame->data) {
    free(frame->data);
    frame->data = nullptr;
  }
}

void scl_free_buffer(void* p) {
  free(p);
}
