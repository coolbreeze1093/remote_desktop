#include "video_convert.h"

#include <cstring>

#include <libyuv/convert_argb.h>
#include <libyuv/convert_from_argb.h>
#include <libyuv/scale.h>

namespace rdc {

namespace {
// libyuv 行例程在宽度对齐时可能向后越界读最多一个向量；紧凑缓冲需要先补齐
// 行距再喂给 libyuv（经验值与旧 core/i420_frame.cc 相同）。
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
      std::memcpy(buf.data() + static_cast<size_t>(r) * stride,
                  src + static_cast<size_t>(r) * srcStride,
                  static_cast<size_t>(width) * 4);
    }
  }
};
}  // namespace

I420Buffer ConvertBgraToI420(const uint8_t* bgra, int srcStrideBytes,
                             int srcW, int srcH, int dstW, int dstH) {
  // 两步转换：
  //   1. ARGBToI420 按源尺寸（平面按 src.width x src.height 分配）
  //   2. I420Scale 缩到编码尺寸
  // libyuv 的 width/height 参数描述源；目标平面必须按此分配，否则越界。
  // 色度行数用 (h+1)/2 兼容奇数高度。
  const size_t srcChromaRows = static_cast<size_t>((srcH + 1) / 2);
  I420Buffer full;
  full.width = srcW;
  full.height = srcH;
  full.y.resize(static_cast<size_t>(srcW) * srcH + kPlaneSlack);
  full.u.resize(static_cast<size_t>(srcW / 2) * srcChromaRows + kPlaneSlack);
  full.v.resize(static_cast<size_t>(srcW / 2) * srcChromaRows + kPlaneSlack);

  PaddedSource padded(bgra, srcStrideBytes, srcW, srcH);
  libyuv::ARGBToI420(padded.buf.data(), padded.stride, full.y.data(), srcW,
                     full.u.data(), srcW / 2, full.v.data(), srcW / 2, srcW,
                     srcH);

  if (srcW == dstW && srcH == dstH) {
    return full;
  }

  const size_t dstChromaRows = static_cast<size_t>((dstH + 1) / 2);
  I420Buffer out;
  out.width = dstW;
  out.height = dstH;
  out.y.resize(static_cast<size_t>(dstW) * dstH + kPlaneSlack);
  out.u.resize(static_cast<size_t>(dstW / 2) * dstChromaRows + kPlaneSlack);
  out.v.resize(static_cast<size_t>(dstW / 2) * dstChromaRows + kPlaneSlack);
  libyuv::I420Scale(full.y.data(), srcW, full.u.data(), srcW / 2,
                    full.v.data(), srcW / 2, srcW, srcH, out.y.data(), dstW,
                    out.u.data(), dstW / 2, out.v.data(), dstW / 2, dstW, dstH,
                    libyuv::kFilterBox);
  return out;
}

I420Buffer ScaleI420(const uint8_t* y, int yStride, const uint8_t* u,
                     int uStride, const uint8_t* v, int vStride, int srcW,
                     int srcH, int dstW, int dstH) {
  const size_t dstChromaRows = static_cast<size_t>((dstH + 1) / 2);
  I420Buffer out;
  out.width = dstW;
  out.height = dstH;
  out.y.resize(static_cast<size_t>(dstW) * dstH + kPlaneSlack);
  out.u.resize(static_cast<size_t>(dstW / 2) * dstChromaRows + kPlaneSlack);
  out.v.resize(static_cast<size_t>(dstW / 2) * dstChromaRows + kPlaneSlack);
  if (srcW == dstW && srcH == dstH) {
    // 尺寸相同：逐行拷贝成紧凑布局（顺带消除源行距差异）。
    for (int r = 0; r < dstH; ++r) {
      std::memcpy(out.y.data() + static_cast<size_t>(r) * dstW,
                  y + static_cast<size_t>(r) * yStride, dstW);
    }
    for (size_t r = 0; r < dstChromaRows; ++r) {
      std::memcpy(out.u.data() + r * (dstW / 2),
                  u + r * static_cast<size_t>(uStride), dstW / 2);
      std::memcpy(out.v.data() + r * (dstW / 2),
                  v + r * static_cast<size_t>(vStride), dstW / 2);
    }
    return out;
  }
  libyuv::I420Scale(y, yStride, u, uStride, v, vStride, srcW, srcH,
                    out.y.data(), dstW, out.u.data(), dstW / 2, out.v.data(),
                    dstW / 2, dstW, dstH, libyuv::kFilterBox);
  return out;
}

void I420ToBgra(const uint8_t* y, int yStride, const uint8_t* u, int uStride,
                const uint8_t* v, int vStride, int width, int height,
                uint8_t* bgraOut, int bgraStrideBytes) {
  // libyuv 的 "ARGB" 输出在小端内存上就是 BGRA。先写进带 padding 的临时
  // 缓冲（自动向量化存储可能越过行尾），再逐行拷给调用方。
  const size_t rowBytes = static_cast<size_t>(width) * 4;
  std::vector<uint8_t> scratch((rowBytes + 4096) * height + 4096);
  const int scratchStride = static_cast<int>(rowBytes + 4096);
  libyuv::I420ToARGB(y, yStride, u, uStride, v, vStride, scratch.data(),
                     scratchStride, width, height);
  for (int r = 0; r < height; ++r) {
    std::memcpy(bgraOut + static_cast<size_t>(r) * bgraStrideBytes,
                scratch.data() + static_cast<size_t>(r) * scratchStride,
                rowBytes);
  }
}

}  // namespace rdc
