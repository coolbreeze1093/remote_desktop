// rdc — OpenH264 编/解码封装（自旧 core/openh264_codec.cc 原样迁移，仅改
// 命名空间与入参类型；编码参数与关键帧策略保持不变）。
#ifndef RDC_SRC_VIDEO_CODEC_H_
#define RDC_SRC_VIDEO_CODEC_H_

#include <cstdint>
#include <vector>

#include "video_convert.h"

struct ISVCEncoder;
struct ISVCDecoder;

namespace rdc {

// Annex-B H.264 编码器。每次 Encode() 产出一个 access unit；关键帧显式请求。
class H264Encoder {
 public:
  H264Encoder() = default;
  ~H264Encoder();
  H264Encoder(const H264Encoder&) = delete;
  H264Encoder& operator=(const H264Encoder&) = delete;

  bool init(int width, int height, int fps, int bitrateKbps);
  void close();

  // 画质阶梯切换时重配（直接重初始化）。下一次 Encode 必须强制 IDR，
  // 解码端才能拿到新 SPS/PPS。
  bool reconfigure(int width, int height, int fps, int bitrateKbps) {
    return init(width, height, fps, bitrateKbps);
  }

  // 编码一帧；返回 Annex-B access unit（跳帧时为空）。forceKey 请求 IDR。
  std::vector<uint8_t> encode(const I420Buffer& frame, bool forceKey);

  int width() const { return width_; }
  int height() const { return height_; }
  int bitrateKbps() const { return bitrateKbps_; }

 private:
  ISVCEncoder* encoder_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int bitrateKbps_ = 0;
};

// Annex-B H.264 解码器。喂 access unit，出完整 I420 帧（紧凑平面 + 实际
// 行距记录在出参）。
class H264Decoder {
 public:
  H264Decoder() = default;
  ~H264Decoder();
  H264Decoder(const H264Decoder&) = delete;
  H264Decoder& operator=(const H264Decoder&) = delete;

  bool init();
  void close();

  // 解码一个 access unit（Annex-B）。返回 true 时 out 内是完整一帧。
  bool decode(const uint8_t* data, size_t size, I420Buffer& out,
              int* yStride, int* uvStride);

 private:
  ISVCDecoder* decoder_ = nullptr;
};

}  // namespace rdc
#endif  // RDC_SRC_VIDEO_CODEC_H_
