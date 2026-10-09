// Repro: capture + preprocess + encode loop (no network).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <libyuv/convert_from_argb.h>
#include <libyuv/scale.h>

#include <screen_capture/capture.hpp>

#include "core/openh264_codec.h"
#include "core/i420_frame.h"

int main(int argc, char** argv) {
  int64_t screenId = (argc > 1) ? strtoll(argv[1], nullptr, 0) : 0;
  rd::H264Encoder enc;
  if (!enc.init(1280, 720, 24, 4000)) {
    printf("encoder init failed\n");
    return 1;
  }
    {  // probe first (mirrors openCapture)
    scl::Options probeOpt;
    auto probe = scl::Capturer::Create(SCL_SOURCE_SCREEN, probeOpt);
    if (probe) probe->Select(screenId);
  }  // destroyed
  scl::Options opt;
  opt.prefer_wgc = false;
  opt.include_cursor = true;
  auto cap = scl::Capturer::Create(SCL_SOURCE_SCREEN, opt);
  if (!cap || !cap->Select(screenId)) {
    printf("capturer failed\n");
    return 1;
  }
  int encoded = 0;
  for (int i = 0; i < 200; ++i) {
    scl::Frame f = cap->Capture(1000);
    if (!f.data) continue;
    rd::I420Frame i420 = rd::ConvertBgraToI420(
        scl_frame{f.width, f.height, f.stride, f.data, f.capture_time_ms,
                  f.top_left_x, f.top_left_y},
        1280, 720);
    std::vector<uint8_t> au = enc.encode(i420, i == 0);
    if (!au.empty()) ++encoded;
    scl_free_buffer(f.data);
    f.data = nullptr;
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }
  printf("encoded %d access units\n", encoded);
  return encoded > 100 ? 0 : 2;
}
