/*
 * screen_capture_lib — C++ facade over the C API.
 *
 * Use this header when you prefer std:: types and want a *persistent*
 * capturer: the underlying WebRTC capturer stays alive between Capture()
 * calls, which is what makes DXGI duplication / WGC sessions efficient.
 * The capturer is not thread-safe; drive it from one thread.
 */
#ifndef SCREEN_CAPTURE_CAPTURE_HPP_
#define SCREEN_CAPTURE_CAPTURE_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "screen_capture/screen_capture.h"

namespace scl {

struct Options {
  bool prefer_wgc = true;             /* Windows Graphics Capture          */
  bool wgc_fallback = true;           /* ...fall back if WGC fails         */
  bool prefer_directx = true;         /* DXGI duplication for screens      */
  bool disable_effects = true;        /* disable window animations         */
  bool include_cursor = true;         /* composite cursor into frames      */
  bool detect_updated_region = false; /* compute changed regions           */

  /* Converts to the underlying C option set (internal use). */
  struct scl_options ToC() const;
};

struct SourceInfo {
  int64_t id = 0;
  std::string title;
};

struct SCL_API Frame {
  int32_t width = 0;
  int32_t height = 0;
  int32_t stride = 0;          /* bytes per row                             */
  uint8_t* data = nullptr;     /* BGRA, top-down; owned by this Frame       */
  int64_t capture_time_ms = 0;
  int64_t top_left_x = 0;      /* origin on the virtual desktop             */
  int64_t top_left_y = 0;

  Frame() = default;
  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;
  Frame(Frame&& other) noexcept { MoveFrom(other); }
  Frame& operator=(Frame&& other) noexcept {
    if (this != &other) {
      Release();
      MoveFrom(other);
    }
    return *this;
  }
  ~Frame() { Release(); }

  /* Deep copy of the pixel buffer packed to width*4 bytes per row
   * (drops the stride padding of the source frame). */
  std::vector<uint8_t> CopyPacked() const;

 private:
  void MoveFrom(Frame& other) noexcept;
  void Release() noexcept;
};

/* Lists screens or windows. Window ids are HWNDs; screen ids are monitor
 * indices, with scl::kScreenAll (-1) meaning the whole virtual desktop. */
constexpr int64_t kScreenAll = SCL_SCREEN_ALL;
constexpr int64_t kScreenDefault = SCL_SCREEN_DEFAULT;

std::vector<SourceInfo> SCL_API EnumSources(scl_source_type type,
                                    const Options& options = Options());

/* Result codes mirror scl_result; use ResultString() for text. */
using Result = scl_result;
inline const char* ResultString(Result r) { return scl_result_string(r); }

/* A persistent capturer for one source type. Select() switches sources on the
 * same capturer instance. Capture() returns:
 *   - a frame on success,
 *   - an empty Frame with result SCL_ERR_TEMPORARY on a transient failure
 *     (just call again),
 *   - an empty Frame with SCL_ERR_PERMANENT if the capturer is broken
 *     (recreate it).
 * timeout_ms bounds the wait for a single capture attempt (default 2000). */
class SCL_API Capturer {
 public:
  static std::unique_ptr<Capturer> Create(scl_source_type type,
                                          const Options& options = Options());

  ~Capturer();
  Capturer(const Capturer&) = delete;
  Capturer& operator=(const Capturer&) = delete;

  /* Enumerates sources valid for this capturer. */
  std::vector<SourceInfo> GetSourceList();

  /* Selects a source returned by GetSourceList(); id semantics as above. */
  bool Select(int64_t id);

  /* Best-effort: brings the selected window to the foreground (windows). */
  bool FocusOnSelectedSource();

  /* Captures exactly one frame; blocks up to timeout_ms. */
  Frame Capture(int timeout_ms = 2000);

  /* Last result of Capture() (SCL_OK on success). */
  Result last_result() const;

  scl_source_type type() const { return type_; }

 private:
  struct Impl;
  /* Defined out-of-line in capture.cc: keeps MSVC from instantiating the
   * unique_ptr deleter body inside this header, where Impl is incomplete. */
  struct ImplDeleter {
    void operator()(Impl* p) const;
  };

  explicit Capturer(scl_source_type type) : type_(type) {}

  std::unique_ptr<Impl, ImplDeleter> impl_;
  scl_source_type type_;
};

}  // namespace scl

#endif /* SCREEN_CAPTURE_CAPTURE_HPP_ */
