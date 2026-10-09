// Minimal repro: ARGBToI420 on a padded copy of a captured frame.
#include <cstdio>
#include <cstring>
#include <vector>

#include <libyuv/convert_from_argb.h>
#include <libyuv/scale.h>

#include "screen_capture/screen_capture.h"

int main() {
  scl_options opt;
  scl_default_options(&opt);
  scl_frame f;
  scl_result r = scl_capture(SCL_SOURCE_SCREEN, 0, &opt, &f);
  printf("capture: %s %dx%d stride=%d\n", scl_result_string(r), f.width,
         f.height, f.stride);
  if (r != SCL_OK) return 1;

  const int pad = 64;
  const int stride = f.width * 4 + pad;
  std::vector<uint8_t> buf(static_cast<size_t>(stride) * f.height + 64, 0);
  for (int y = 0; y < f.height; ++y) {
    memcpy(buf.data() + y * stride, f.data + y * f.stride,
           static_cast<size_t>(f.width) * 4);
  }

  std::vector<uint8_t> Y(static_cast<size_t>(f.width) * f.height);
  std::vector<uint8_t> U(static_cast<size_t>(f.width / 2) * (f.height / 2));
  std::vector<uint8_t> V(static_cast<size_t>(f.width / 2) * (f.height / 2));
  printf("calling ARGBToI420...\n");
  fflush(stdout);
  libyuv::ARGBToI420(buf.data(), stride, Y.data(), f.width, U.data(), f.width / 2,
                     V.data(), f.width / 2, f.width, f.height);
  printf("ARGBToI420 ok, Y[0]=%d\n", Y[0]);

  std::vector<uint8_t> Y2(static_cast<size_t>(1280) * 720);
  std::vector<uint8_t> U2(static_cast<size_t>(640) * 360);
  std::vector<uint8_t> V2(static_cast<size_t>(640) * 360);
  printf("calling I420Scale...\n");
  fflush(stdout);
  libyuv::I420Scale(Y.data(), f.width, U.data(), f.width / 2, V.data(),
                    f.width / 2, f.width, f.height, Y2.data(), 1280, U2.data(),
                    640, V2.data(), 640, 1280, 720, libyuv::kFilterBox);
  printf("I420Scale ok\n");
  scl_frame_free(&f);
  printf("DONE\n");
  return 0;
}
