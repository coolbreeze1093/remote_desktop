/*
 * screen_capture_lib — standalone screen/window capture library for Windows.
 *
 * Extracted from the WebRTC project's modules/desktop_capture (BSD-licensed,
 * see LICENSE.webrtc in the vendored/ directory). Built independently with
 * CMake; has no dependency on any other part of WebRTC at link time.
 *
 * The C API below is the stable entry point. A thin C++ facade is available
 * in screen_capture/capture.hpp for callers that prefer std:: types.
 *
 * Pixel format: 32-bit BGRA (like WebRTC DesktopFrame), top-down rows,
 * `stride` bytes per row (stride >= width * 4).
 */
#ifndef SCREEN_CAPTURE_SCREEN_CAPTURE_H_
#define SCREEN_CAPTURE_SCREEN_CAPTURE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SCL_API is for the DLL build; the static build needs nothing. */
#if defined(SCL_BUILDING_DLL)
#define SCL_API __declspec(dllexport)
#elif defined(SCL_DLL)
#define SCL_API __declspec(dllimport)
#else
#define SCL_API
#endif

/* ---------------------------------------------------------------- version */

SCL_API const char* scl_version_string(void);

/* ------------------------------------------------------------- error codes */

typedef enum scl_result {
  SCL_OK = 0,
  SCL_ERR_INVALID_ARG = -1,   /* null pointer / bad enum / bad arguments    */
  SCL_ERR_NOT_FOUND = -2,     /* source id unknown (window closed etc.)     */
  SCL_ERR_TEMPORARY = -3,     /* retry later (mode switch, UAC window, ...) */
  SCL_ERR_PERMANENT = -4,     /* capture path is broken, do not retry       */
  SCL_ERR_UNSUPPORTED = -5,   /* e.g. WGC requested but OS cannot provide   */
  SCL_ERR_OUT_OF_MEMORY = -6
} scl_result;

SCL_API const char* scl_result_string(scl_result r);

/* ------------------------------------------------------------ source types */

typedef enum scl_source_type {
  SCL_SOURCE_SCREEN = 0,      /* monitors; use SCL_SCREEN_ALL for all       */
  SCL_SOURCE_WINDOW = 1       /* top-level windows                          */
} scl_source_type;

/* Special screen id: capture the entire virtual desktop (all monitors). */
#define SCL_SCREEN_ALL ((int64_t)-1)
/* Special screen id: let the implementation pick the primary screen. */
#define SCL_SCREEN_DEFAULT ((int64_t)0)

/* --------------------------------------------------------------- options */

typedef struct scl_options {
  /* Windows Graphics Capture (Win10 1903+, robust, captures protected-ish
   * content better; requires Win11+ for some window paths). 0=off, 1=on. */
  int prefer_wgc;
  /* When WGC fails and wgc_fallback is set, fall back to the GDI/DXGI path. */
  int wgc_fallback;
  /* Use DXGI desktop duplication for screens, with GDI as fallback. */
  int prefer_directx;
  /* Disable window animations / Aero peek during capture. Default 1. */
  int disable_effects;
  /* Composite the mouse cursor into returned frames. Default 1. */
  int include_cursor;
  /* Compute only-changed regions (slower per frame; useful for streaming). */
  int detect_updated_region;
  /* Max wait for a single capture attempt, milliseconds. Default 2000. */
  int capture_timeout_ms;
} scl_options;

SCL_API scl_result scl_default_options(scl_options* out);

/* ---------------------------------------------------------- source listing */

typedef struct scl_source_info {
  int64_t id;         /* screen index or HWND as integer; stable per type  */
  const char* title;  /* UTF-8, may be ""; owned by the library, valid
                       * until the next scl_enum_* call on this type.      */
} scl_source_info;

/* Two-phase enumeration:
 *   int32_t n = 0;
 *   scl_enum_sources(SCL_SOURCE_WINDOW, NULL, NULL, &n);   // -> n
 *   std::vector<scl_source_info> v(n);
 *   scl_enum_sources(SCL_SOURCE_WINDOW, NULL, v.data(), &n);
 * `opt` may be NULL (defaults are used). */
SCL_API scl_result scl_enum_sources(scl_source_type type,
                                    const scl_options* opt,
                                    scl_source_info* buf,
                                    int32_t* count);

/* -------------------------------------------------------------- capture */

typedef struct scl_frame {
  int32_t width;
  int32_t height;
  int32_t stride;          /* bytes per row, >= width * 4                  */
  uint8_t* data;           /* BGRA, top-down; owned by the caller          */
  int64_t capture_time_ms; /* wall clock of the capture in ms              */
  int64_t top_left_x;      /* frame origin on the virtual desktop          */
  int64_t top_left_y;
} scl_frame;

/* Capture one frame of a screen or window.
 *
 * Every call is self-contained (creates a capturer, selects the source,
 * captures once, destroys the capturer) and is safe to call from any thread.
 * For continuous capture prefer the C++ facade (capture.hpp), which keeps the
 * underlying capturer alive between frames.
 *
 * On SCL_OK the caller owns out->data and must release it with
 * scl_frame_free(). On failure out->data is NULL. */
SCL_API scl_result scl_capture(scl_source_type type,
                               int64_t source_id,
                               const scl_options* opt,
                               scl_frame* out);

SCL_API void scl_frame_free(scl_frame* frame);

/* Frees any buffer the library handed out (e.g. scl_frame.data). The
 * allocation and the free always happen inside the library, so this is safe
 * across CRT/DLL boundaries. */
SCL_API void scl_free_buffer(void* p);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SCREEN_CAPTURE_SCREEN_CAPTURE_H_ */
