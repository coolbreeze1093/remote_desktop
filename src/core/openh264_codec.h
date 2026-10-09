// remote_desktop — OpenH264 encoder/decoder wrappers.
#ifndef REMOTE_DESKTOP_CORE_OPENH264_CODEC_H_
#define REMOTE_DESKTOP_CORE_OPENH264_CODEC_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "core/i420_frame.h"

struct ISVCEncoder;
struct ISVCDecoder;

namespace rd {

// Annex-B H.264 encoder (OpenH264). Produces one access unit per Encode()
// call; keyframes are requested explicitly.
class H264Encoder {
 public:
  H264Encoder() = default;
  ~H264Encoder();
  H264Encoder(const H264Encoder&) = delete;
  H264Encoder& operator=(const H264Encoder&) = delete;

  bool init(int width, int height, int fps, int bitrateKbps);
  void close();

  // Re-inits with new parameters (dynamic quality ladder). Cheap enough to
  // call on occasional level changes; the next encode must be forced to an IDR
  // so the decoder picks up the new SPS/PPS.
  bool reconfigure(int width, int height, int fps, int bitrateKbps) {
    return init(width, height, fps, bitrateKbps);
  }

  // Encodes one frame; returns the Annex-B access unit (empty on skip).
  // forceKey asks the encoder to emit an IDR.
  std::vector<uint8_t> encode(const I420Frame& frame, bool forceKey);

  int width() const { return width_; }
  int height() const { return height_; }
  int bitrateKbps() const { return bitrateKbps_; }

 private:
  ISVCEncoder* encoder_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int bitrateKbps_ = 0;
};

// Annex-B H.264 decoder (OpenH264). Feed access units; full frames come out.
class H264Decoder {
 public:
  H264Decoder() = default;
  ~H264Decoder();
  H264Decoder(const H264Decoder&) = delete;
  H264Decoder& operator=(const H264Decoder&) = delete;

  bool init();
  void close();

  // Decodes one access unit (Annex-B). Returns true when a complete frame is
  // available in `out` (YUV planes, stride in out strides).
  bool decode(const uint8_t* data, size_t size, I420Frame& out,
              std::vector<uint8_t>& yBuf, std::vector<uint8_t>& uBuf,
              std::vector<uint8_t>& vBuf, int& yStride, int& uvStride);

 private:
  ISVCDecoder* decoder_ = nullptr;
};

}  // namespace rd

#endif
