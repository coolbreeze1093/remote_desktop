/*
 * scl_demo — minimal exercise of the screen_capture_lib public API.
 *
 *   scl_demo.exe                 -> enumerate screens + windows, capture the
 *                                   primary screen 3x, write BMP files
 *   scl_demo.exe window <hwnd>   -> capture the given window once
 *   scl_demo.exe screen <id>     -> capture the given screen once
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <chrono>
#include <thread>

#include "screen_capture/screen_capture.h"
#include "screen_capture/capture.hpp"

namespace {

/* 24-bit BMP writer for the BGRA frames (drops the alpha byte). */
bool WriteBmp(const char* path, const scl::Frame& f) {
  if (!f.data || f.width <= 0 || f.height <= 0) {
    return false;
  }
  const int32_t w = f.width;
  const int32_t h = f.height;
  const size_t row = ((static_cast<size_t>(w) * 3 + 3) / 4) * 4;
  const size_t pixels = row * static_cast<size_t>(h);
  const size_t file_size = 54 + pixels;

  std::vector<uint8_t> buf(file_size, 0);
  auto put16 = [&](size_t off, uint16_t v) { memcpy(&buf[off], &v, 2); };
  auto put32 = [&](size_t off, uint32_t v) { memcpy(&buf[off], &v, 4); };

  buf[0] = 'B'; buf[1] = 'M';
  put32(2, static_cast<uint32_t>(file_size));
  put32(10, 54);
  put32(14, 40);            /* BITMAPINFOHEADER */
  put32(18, static_cast<uint32_t>(w));
  put32(22, static_cast<uint32_t>(h));  /* positive: bottom-up */
  put16(26, 1);
  put16(28, 24);
  put32(34, static_cast<uint32_t>(pixels));

  for (int32_t y = 0; y < h; ++y) {
    /* top-down source -> bottom-up BMP */
    const uint8_t* src = f.data + static_cast<size_t>(h - 1 - y) * f.stride;
    uint8_t* dst = &buf[54] + row * static_cast<size_t>(y);
    for (int32_t x = 0; x < w; ++x) {
      dst[x * 3 + 0] = src[x * 4 + 0];
      dst[x * 3 + 1] = src[x * 4 + 1];
      dst[x * 3 + 2] = src[x * 4 + 2];
    }
  }
  FILE* fp = fopen(path, "wb");
  if (!fp) {
    return false;
  }
  const bool ok = fwrite(buf.data(), 1, buf.size(), fp) == buf.size();
  fclose(fp);
  return ok;
}

void PrintSources(scl_source_type type) {
  int32_t n = 0;
  if (scl_enum_sources(type, nullptr, nullptr, &n) != SCL_OK) {
    printf("  enumeration failed\n");
    return;
  }
  if (n == 0) {
    printf("  (none)\n");
    return;
  }
  std::vector<scl_source_info> infos(static_cast<size_t>(n));
  int32_t cap = n;
  scl_enum_sources(type, nullptr, infos.data(), &cap);
  printf("  %d source(s):\n", n);
  for (int32_t i = 0; i < n; ++i) {
    printf("    id=%lld  title=%s\n",
           static_cast<long long>(infos[i].id),
           infos[i].title && infos[i].title[0] ? infos[i].title : "(untitled)");
  }
}

/* Persistent-capturer path (C++ facade). */
int DemoScreen(int64_t screen_id, int frames) {
  scl::Options opt;
  opt.include_cursor = true;
  auto cap = scl::Capturer::Create(SCL_SOURCE_SCREEN, opt);
  if (!cap) {
    fprintf(stderr, "Capturer::Create failed\n");
    return 1;
  }
  printf("screens:\n");
  for (const auto& s : cap->GetSourceList()) {
    printf("  id=%lld  title=%s\n", static_cast<long long>(s.id),
           s.title.empty() ? "(untitled)" : s.title.c_str());
  }
  if (!cap->Select(screen_id)) {
    fprintf(stderr, "Select(%lld) failed: %s\n", static_cast<long long>(screen_id),
            scl::ResultString(cap->last_result()));
    return 1;
  }
  for (int i = 0; i < frames; ++i) {
    scl::Frame f = cap->Capture(2000);
    if (!f.data) {
      fprintf(stderr, "frame %d: %s\n", i, scl::ResultString(cap->last_result()));
      return 1;
    }
    char path[64];
    snprintf(path, sizeof(path), "frame_%02d.bmp", i);
    if (WriteBmp(path, f)) {
      printf("frame %d: %dx%d stride=%d -> %s (t=%lldms, origin=%lld,%lld)\n",
             i, f.width, f.height, f.stride, path,
             static_cast<long long>(f.capture_time_ms),
             static_cast<long long>(f.top_left_x),
             static_cast<long long>(f.top_left_y));
    } else {
      fprintf(stderr, "frame %d: BMP write failed\n", i);
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
  }
  return 0;
}

/* One-shot path (C API). */
int DemoWindow(int64_t hwnd_id) {
  scl_options opt;
  scl_default_options(&opt);
  scl_frame f;
  scl_result r = scl_capture(SCL_SOURCE_WINDOW, hwnd_id, &opt, &f);
  if (r != SCL_OK) {
    fprintf(stderr, "scl_capture(window) failed: %s\n", scl_result_string(r));
    return 1;
  }
  scl::Frame owned;
  owned.width = f.width; owned.height = f.height; owned.stride = f.stride;
  owned.data = f.data; owned.capture_time_ms = f.capture_time_ms;
  owned.top_left_x = f.top_left_x; owned.top_left_y = f.top_left_y;
  if (WriteBmp("window.bmp", owned)) {
    printf("window %lld: %dx%d -> window.bmp\n", static_cast<long long>(hwnd_id),
           f.width, f.height);
  }
  scl_frame_free(&f);  /* Frame::Release was NOT used; buffer freed once here */
  owned.data = nullptr;
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  printf("%s\n", scl_version_string());

  printf("screens:\n");
  PrintSources(SCL_SOURCE_SCREEN);
  printf("windows (first 15):\n");
  PrintSources(SCL_SOURCE_WINDOW);

  if (argc >= 3 && strcmp(argv[1], "window") == 0) {
    return DemoWindow(strtoll(argv[2], nullptr, 0));
  }
  if (argc >= 3 && strcmp(argv[1], "screen") == 0) {
    return DemoScreen(strtoll(argv[2], nullptr, 0), 1);
  }

  /* Default flow: 3 frames of the primary screen via the persistent capturer. */
  int rc = DemoScreen(SCL_SCREEN_DEFAULT, 3);
  printf("%s\n", rc == 0 ? "DEMO OK" : "DEMO FAILED");
  return rc;
}
