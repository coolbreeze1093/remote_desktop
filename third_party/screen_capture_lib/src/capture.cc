/*
 * screen_capture_lib — implementation of the C++ facade.
 */
#include "screen_capture/capture.hpp"

#include <cstring>

#include "capture_internal.h"

namespace scl {

/* ------------------------------------------------------------- Options */

struct scl_options Options::ToC() const {
  struct scl_options o{};
  o.prefer_wgc = prefer_wgc ? 1 : 0;
  o.wgc_fallback = wgc_fallback ? 1 : 0;
  o.prefer_directx = prefer_directx ? 1 : 0;
  o.disable_effects = disable_effects ? 1 : 0;
  o.include_cursor = include_cursor ? 1 : 0;
  o.detect_updated_region = detect_updated_region ? 1 : 0;
  o.capture_timeout_ms = 0; /* the facade bounds the wait per Capture() call */
  return o;
}

/* --------------------------------------------------------------- Frame */

std::vector<uint8_t> Frame::CopyPacked() const {
  std::vector<uint8_t> out;
  if (!data || width <= 0 || height <= 0) {
    return out;
  }
  const size_t packed = static_cast<size_t>(width) * 4;
  out.resize(packed * static_cast<size_t>(height));
  for (int32_t y = 0; y < height; ++y) {
    memcpy(out.data() + packed * y, data + stride * y, packed);
  }
  return out;
}

void Frame::MoveFrom(Frame& other) noexcept {
  width = other.width;
  height = other.height;
  stride = other.stride;
  data = other.data;
  capture_time_ms = other.capture_time_ms;
  top_left_x = other.top_left_x;
  top_left_y = other.top_left_y;
  other.width = other.height = other.stride = 0;
  other.data = nullptr;
  other.capture_time_ms = 0;
  other.top_left_x = other.top_left_y = 0;
}

void Frame::Release() noexcept {
  if (data) {
    scl_free_buffer(data);
    data = nullptr;
  }
  width = height = stride = 0;
}

/* ---------------------------------------------------------- EnumSources */

std::vector<SourceInfo> EnumSources(scl_source_type type, const Options& options) {
  std::vector<SourceInfo> out;
  if (type != SCL_SOURCE_SCREEN && type != SCL_SOURCE_WINDOW) {
    return out;
  }
  struct scl_options c = options.ToC();
  int32_t n = 0;
  if (scl_enum_sources(type, &c, nullptr, &n) != SCL_OK || n <= 0) {
    return out;
  }
  std::vector<scl_source_info> infos(static_cast<size_t>(n));
  int32_t capacity = static_cast<int32_t>(infos.size());
  if (scl_enum_sources(type, &c, infos.data(), &capacity) != SCL_OK) {
    return out;
  }
  out.reserve(static_cast<size_t>(n));
  for (int32_t i = 0; i < n; ++i) {
    out.push_back(SourceInfo{infos[i].id,
                             infos[i].title ? infos[i].title : ""});
  }
  return out;
}

/* ------------------------------------------------------------- Capturer */

struct Capturer::Impl {
  explicit Impl(const Options& opts)
      : options(opts.ToC()) {}

  struct scl_options options;
  /* Declared before `capturer` on purpose: the capturer holds a raw Callback*
   * into `cb`, so `capturer` must be destroyed first (reverse order). */
  scl_internal::BlockingCallback cb;
  std::unique_ptr<webrtc::DesktopCapturer> capturer;
  scl_result last_result = SCL_ERR_INVALID_ARG;
};

std::unique_ptr<Capturer> Capturer::Create(scl_source_type type,
                                           const Options& options) {
  if (type != SCL_SOURCE_SCREEN && type != SCL_SOURCE_WINDOW) {
    return nullptr;
  }
  std::unique_ptr<Capturer> self(new Capturer(type));
  self->impl_ = std::unique_ptr<Impl, ImplDeleter>(new Impl(options));
  self->impl_->capturer = scl_internal::CreateCapturer(
      type, scl_internal::ToWebrtcOptions(&self->impl_->options),
      /*include_cursor=*/true);
  if (!self->impl_->capturer) {
    return nullptr;
  }
  self->impl_->capturer->Start(&self->impl_->cb);
  return self;
}

Capturer::~Capturer() = default;

void Capturer::ImplDeleter::operator()(Impl* p) const {
  delete p;
}

std::vector<SourceInfo> Capturer::GetSourceList() {
  std::vector<SourceInfo> out;
  if (!impl_ || !impl_->capturer) {
    return out;
  }
  webrtc::DesktopCapturer::SourceList sources;
  if (!impl_->capturer->GetSourceList(&sources)) {
    return out;
  }
  out.reserve(sources.size());
  for (const auto& s : sources) {
    out.push_back(SourceInfo{static_cast<int64_t>(s.id), s.title});
  }
  return out;
}

bool Capturer::Select(int64_t id) {
  return impl_ && impl_->capturer &&
         impl_->capturer->SelectSource(
             static_cast<webrtc::DesktopCapturer::SourceId>(id));
}

bool Capturer::FocusOnSelectedSource() {
  return impl_ && impl_->capturer && impl_->capturer->FocusOnSelectedSource();
}

Result Capturer::last_result() const {
  return impl_ ? impl_->last_result : SCL_ERR_INVALID_ARG;
}

Frame Capturer::Capture(int timeout_ms) {
  Frame f;
  if (!impl_ || !impl_->capturer) {
    if (impl_) {
      impl_->last_result = SCL_ERR_INVALID_ARG;
    }
    return f;
  }
  impl_->last_result = SCL_ERR_TEMPORARY;
  impl_->cb.Reset();
  impl_->capturer->CaptureFrame();
  if (!impl_->cb.Wait(timeout_ms)) {
    return f;
  }
  impl_->last_result = scl_internal::MapResult(impl_->cb.result());
  if (impl_->last_result != SCL_OK) {
    return f;
  }
  std::unique_ptr<webrtc::DesktopFrame> frame = impl_->cb.TakeFrame();
  if (!frame) {
    impl_->last_result = SCL_ERR_PERMANENT;
    return f;
  }
  scl_frame raw{};
  impl_->last_result = scl_internal::CopyFrameOut(*frame, &raw);
  if (impl_->last_result != SCL_OK) {
    return f;
  }
  f.width = raw.width;
  f.height = raw.height;
  f.stride = raw.stride;
  f.data = raw.data;
  f.capture_time_ms = raw.capture_time_ms;
  f.top_left_x = raw.top_left_x;
  f.top_left_y = raw.top_left_y;
  return f;
}

}  // namespace scl
