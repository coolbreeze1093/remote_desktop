// rdc — 视频像素格式转换（libyuv）。引擎内部使用；I420ToBgra 经 engine.h
// 对宿主导出。
#ifndef RDC_SRC_VIDEO_CONVERT_H_
#define RDC_SRC_VIDEO_CONVERT_H_

#include <cstdint>
#include <vector>

namespace rdc {

// 紧凑 I420 平面缓冲（y 行距 = width，uv 行距 = width/2；尾部留 slack 防
// libyuv 向量化越界读）。
struct I420Buffer {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> y;
  std::vector<uint8_t> u;
  std::vector<uint8_t> v;
};

// BGRA（内存序，同 libyuv "ARGB"）→ I420，缩放到 dstW x dstH（两步法：
// 先按源尺寸转，再 I420Scale，详见实现内注释）。
I420Buffer ConvertBgraToI420(const uint8_t* bgra, int srcStrideBytes,
                             int srcW, int srcH, int dstW, int dstH);

// I420 → I420 缩放（源行距任意）。尺寸相当时退化为带行距归一的拷贝。
I420Buffer ScaleI420(const uint8_t* y, int yStride, const uint8_t* u,
                     int uStride, const uint8_t* v, int vStride, int srcW,
                     int srcH, int dstW, int dstH);

}  // namespace rdc
#endif  // RDC_SRC_VIDEO_CONVERT_H_
