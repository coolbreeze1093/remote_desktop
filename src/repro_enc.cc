// Repro: capture + preprocess + encode loop (no network).
// 已迁移到 rdc 引擎：编解码/转换用引擎封装（随 rdc 静态库链接），
// src/core 里的重复实现已删除。
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include <screen_capture/capture.hpp>

#include "video_codec.h"
#include "video_convert.h"

int main(int argc, char** argv) {
  int64_t screenId = (argc > 1) ? strtoll(argv[1], nullptr, 0) : 0;
  rdc::H264Encoder enc;
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
    rdc::I420Buffer i420 = rdc::ConvertBgraToI420(
        f.data, f.stride, f.width, f.height, 1280, 720);
    std::vector<uint8_t> au = enc.encode(i420, i == 0);
    if (!au.empty()) ++encoded;
    scl_free_buffer(f.data);
    f.data = nullptr;
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }
  printf("encoded %d access units\n", encoded);
  return encoded > 100 ? 0 : 2;
}
