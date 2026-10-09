// Repro #2: persistent capturer + threaded convert loop (mirrors self test).
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <libyuv/convert_from_argb.h>
#include <libyuv/scale.h>

#include <screen_capture/capture.hpp>

int main(int argc, char** argv) {
  int64_t screenId = (argc > 1) ? strtoll(argv[1], nullptr, 0) : 0;
  // Mirror the self test: a temporary capturer is created and destroyed first
  // (RemoteDesktopSession::openCapture does this as a validity probe).
  {
    scl::Options probe;
    auto probeCap = scl::Capturer::Create(SCL_SOURCE_SCREEN, probe);
    if (probeCap) probeCap->Select(screenId);
  }  // destroyed here

  scl::Options opt;
  opt.prefer_wgc = false;
  opt.include_cursor = true;
  auto cap = scl::Capturer::Create(SCL_SOURCE_SCREEN, opt);
  if (!cap || !cap->Select(screenId)) {
    printf("capturer create/select failed\n");
    return 1;
  }
  std::atomic_bool run{true};
  int converted = 0;
  std::thread t([&] {
    while (run) {
      scl::Frame f = cap->Capture(1000);
      if (!f.data) continue;
      // padded copy
      const int pad = 64;
      const int stride = f.width * 4 + pad;
      std::vector<uint8_t> buf(static_cast<size_t>(stride) * f.height + 64, 0);
      for (int y = 0; y < f.height; ++y) {
        memcpy(buf.data() + static_cast<size_t>(y) * stride,
               f.data + static_cast<size_t>(y) * f.stride,
               static_cast<size_t>(f.width) * 4);
      }
      std::vector<uint8_t> Y(static_cast<size_t>(f.width) * f.height);
      std::vector<uint8_t> U(static_cast<size_t>(f.width / 2) * (f.height / 2));
      std::vector<uint8_t> V(static_cast<size_t>(f.width / 2) * (f.height / 2));
      libyuv::ARGBToI420(buf.data(), stride, Y.data(), f.width, U.data(),
                         f.width / 2, V.data(), f.width / 2, f.width, f.height);
      std::vector<uint8_t> Y2(static_cast<size_t>(1280) * 720);
      std::vector<uint8_t> U2(static_cast<size_t>(640) * 360);
      std::vector<uint8_t> V2(static_cast<size_t>(640) * 360);
      libyuv::I420Scale(Y.data(), f.width, U.data(), f.width / 2, V.data(),
                        f.width / 2, f.width, f.height, Y2.data(), 1280,
                        U2.data(), 640, V2.data(), 640, 1280, 720,
                        libyuv::kFilterBox);
      ++converted;
      if (converted % 24 == 0) printf("converted %d frames\n", converted);
      scl_free_buffer(f.data);
      f.data = nullptr;
    }
  });
  // Let it run ~12s like the self test window.
  std::this_thread::sleep_for(std::chrono::seconds(12));
  run = false;
  t.join();
  printf("total converted: %d\n", converted);
  return converted > 0 ? 0 : 2;
}
