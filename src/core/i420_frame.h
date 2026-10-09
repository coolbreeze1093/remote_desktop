// remote_desktop — I420 frame container + converter from screen_capture_lib
// BGRA frames via libyuv.
#ifndef REMOTE_DESKTOP_CORE_I420_FRAME_H_
#define REMOTE_DESKTOP_CORE_I420_FRAME_H_

#include <cstdint>
#include <vector>

#include "screen_capture/screen_capture.h"

namespace rd {

// Standard I420 (YUV 4:2:0 planar) frame.
struct I420Frame {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> y;
  std::vector<uint8_t> u;
  std::vector<uint8_t> v;

  bool empty() const { return y.empty(); }
  size_t ySize() const {
    return static_cast<size_t>(width) * static_cast<size_t>(height);
  }
  size_t uvSize() const {
    return static_cast<size_t>(width / 2) * static_cast<size_t>(height / 2);
  }
};

// BGRA (screen_capture_lib) -> I420, scaled to fixed encode dimensions.
I420Frame ConvertBgraToI420(const scl_frame& src, int dstW, int dstH);

// I420 (padded planes from the decoder) -> packed BGRA for display.
void I420ToBgra(const uint8_t* y, int yStride, const uint8_t* u, int uStride,
                const uint8_t* v, int vStride, int width, int height,
                uint8_t* bgraOut, int bgraStrideBytes);

}  // namespace rd

#endif
