#include "core/i420_frame.h"

#include <cstring>
#include <vector>

#include <libyuv/convert_argb.h>
#include <libyuv/convert_from_argb.h>
#include <libyuv/scale.h>

namespace rd {

namespace {
// libyuv row routines read past the last pixel of a row by up to one vector
// when the width is aligned; WebRTC DesktopFrames always carry row padding,
// tightly packed scl buffers do not. Copy the source once with row padding.
constexpr int kRowPadding = 64;
constexpr size_t kPlaneSlack = 4096;

struct PaddedSource {
  std::vector<uint8_t> buf;
  int stride = 0;

  PaddedSource(const uint8_t* src, int srcStride, int width, int height) {
    stride = width * 4 + kRowPadding;
    buf.resize(static_cast<size_t>(stride) * static_cast<size_t>(height) + 64,
               0);
    for (int r = 0; r < height; ++r) {
      memcpy(buf.data() + static_cast<size_t>(r) * stride,
             src + static_cast<size_t>(r) * srcStride,
             static_cast<size_t>(width) * 4);
    }
  }
};
}  // namespace

I420Frame ConvertBgraToI420(const scl_frame& src, int dstW, int dstH) {
  /*
   * Two-step conversion:
   *   1. ARGBToI420 at the SOURCE size (planes sized src.width x src.height)
   *   2. I420Scale down to the encode size (dstW x dstH)
   * libyuv's width/height parameters describe the SOURCE; the destination
   * planes must be sized accordingly. Sizing them for the encode size while
   * passing source dimensions overruns every plane.
   */
  I420Frame full;
  full.width = src.width;
  full.height = src.height;
  full.y.resize(full.ySize() + kPlaneSlack);
  full.u.resize(full.uvSize() + kPlaneSlack);
  full.v.resize(full.uvSize() + kPlaneSlack);

  // ARGB in libyuv terms = little-endian BGRA in memory, which is what
  // screen_capture_lib produces.
  PaddedSource padded(src.data, src.stride, src.width, src.height);
  libyuv::ARGBToI420(padded.buf.data(), padded.stride, full.y.data(),
                     src.width, full.u.data(), src.width / 2, full.v.data(),
                     src.width / 2, src.width, src.height);

  I420Frame out;
  out.width = dstW;
  out.height = dstH;
  out.y.resize(out.ySize() + kPlaneSlack);
  out.u.resize(out.uvSize() + kPlaneSlack);
  out.v.resize(out.uvSize() + kPlaneSlack);

  if (src.width != dstW || src.height != dstH) {
    libyuv::I420Scale(full.y.data(), src.width, full.u.data(), src.width / 2,
                      full.v.data(), src.width / 2, src.width, src.height,
                      out.y.data(), dstW, out.u.data(), dstW / 2, out.v.data(),
                      dstW / 2, dstW, dstH, libyuv::kFilterBox);
  } else {
    out = std::move(full);
  }
  return out;
}

void I420ToBgra(const uint8_t* y, int yStride, const uint8_t* u, int uStride,
                const uint8_t* v, int vStride, int width, int height,
                uint8_t* bgraOut, int bgraStrideBytes) {
  // libyuv "ARGB" output buffers are little-endian BGRA in memory. Convert
  // into a padded scratch buffer (auto-vectorized stores may overrun row
  // ends), then copy rows into the caller's image.
  const size_t rowBytes = static_cast<size_t>(width) * 4;
  std::vector<uint8_t> scratch((rowBytes + 4096) * height + 4096);
  const int scratchStride = static_cast<int>(rowBytes + 4096);
  libyuv::I420ToARGB(y, yStride, u, uStride, v, vStride, scratch.data(),
                     scratchStride, width, height);
  for (int r = 0; r < height; ++r) {
    memcpy(bgraOut + static_cast<size_t>(r) * bgraStrideBytes,
           scratch.data() + static_cast<size_t>(r) * scratchStride, rowBytes);
  }
}

}  // namespace rd
