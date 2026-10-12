#include "video_codec.h"

#include <cstring>

#include "codec_api.h"

namespace rdc {

// ----------------------------------------------------------------- encoder

H264Encoder::~H264Encoder() { close(); }

bool H264Encoder::init(int width, int height, int fps, int bitrateKbps) {
  close();
  if (WelsCreateSVCEncoder(&encoder_) != 0 || !encoder_) {
    return false;
  }
  width_ = width;
  height_ = height;
  bitrateKbps_ = bitrateKbps;

  SEncParamExt param;
  memset(&param, 0, sizeof(param));
  encoder_->GetDefaultParams(&param);
  param.iUsageType = SCREEN_CONTENT_REAL_TIME;
  param.iPicWidth = width;
  param.iPicHeight = height;
  param.fMaxFrameRate = static_cast<float>(fps);
  param.iTargetBitrate = bitrateKbps * 1000;
  param.iRCMode = RC_BITRATE_MODE;
  param.bEnableFrameSkip = false;
  // IDR every 2s: a decoder joining mid-GOP or losing the IDR (UDP) must not
  // stay black until the next on-demand keyframe. Periodic IDR costs ~20KB/2s
  // at 4Mbps — negligible.
  param.uiIntraPeriod = fps * 2;
  param.eSpsPpsIdStrategy = CONSTANT_ID;
  // Single thread: OpenH264's screen-content mode does not parallelize
  // (measured: 4 threads made it slower); the ladder trades fps for pixels
  // instead.
  param.iMultipleThreadIdc = 1;
  param.iSpatialLayerNum = 1;
  param.iTemporalLayerNum = 1;
  param.sSpatialLayers[0].iVideoWidth = width;
  param.sSpatialLayers[0].iVideoHeight = height;
  param.sSpatialLayers[0].fFrameRate = static_cast<float>(fps);
  param.sSpatialLayers[0].iSpatialBitrate = bitrateKbps * 1000;
  param.sSpatialLayers[0].uiProfileIdc = PRO_BASELINE;

  if (encoder_->InitializeExt(&param) != cmResultSuccess) {
    close();
    return false;
  }
  int videoFormat = videoFormatI420;
  encoder_->SetOption(ENCODER_OPTION_DATAFORMAT, &videoFormat);
  return true;
}

void H264Encoder::close() {
  if (encoder_) {
    encoder_->Uninitialize();
    WelsDestroySVCEncoder(encoder_);
    encoder_ = nullptr;
  }
  width_ = height_ = 0;
  bitrateKbps_ = 0;
}

std::vector<uint8_t> H264Encoder::encode(const I420Buffer& frame,
                                         bool forceKey) {
  if (!encoder_ || frame.width != width_ || frame.height != height_) {
    return {};
  }
  if (forceKey) {
    // Request an IDR for the next frame (do NOT touch IDR_INTERVAL:
    // setting it to 1 would make every frame a keyframe).
    encoder_->ForceIntraFrame(true);
  }

  SSourcePicture pic;
  memset(&pic, 0, sizeof(pic));
  pic.iPicWidth = frame.width;
  pic.iPicHeight = frame.height;
  pic.iColorFormat = videoFormatI420;
  pic.iStride[0] = frame.width;
  pic.iStride[1] = pic.iStride[2] = frame.width / 2;
  pic.pData[0] = const_cast<uint8_t*>(frame.y.data());
  pic.pData[1] = const_cast<uint8_t*>(frame.u.data());
  pic.pData[2] = const_cast<uint8_t*>(frame.v.data());

  SFrameBSInfo info;
  memset(&info, 0, sizeof(info));
  if (encoder_->EncodeFrame(&pic, &info) != cmResultSuccess) {
    return {};
  }
  if (info.iFrameSizeInBytes <= 0) {
    return {};
  }

  // Flatten the layer/segment NAL layout into one Annex-B access unit.
  std::vector<uint8_t> au;
  au.reserve(static_cast<size_t>(info.iFrameSizeInBytes) + 64);
  static const uint8_t kStartCode[4] = {0, 0, 0, 1};
  for (int layer = 0; layer < info.iLayerNum; ++layer) {
    const SLayerBSInfo& bs = info.sLayerInfo[layer];
    const uint8_t* layerPtr = bs.pBsBuf;
    size_t total = 0;
    for (int nal = 0; nal < bs.iNalCount; ++nal) {
      total += static_cast<size_t>(bs.pNalLengthInByte[nal]);
    }
    au.insert(au.end(), kStartCode, kStartCode + 4);
    au.insert(au.end(), layerPtr, layerPtr + total);
  }
  return au;
}

// ----------------------------------------------------------------- decoder

H264Decoder::~H264Decoder() { close(); }

bool H264Decoder::init() {
  close();
  if (WelsCreateDecoder(&decoder_) != 0 || !decoder_) {
    return false;
  }
  SDecodingParam param;
  memset(&param, 0, sizeof(param));
  param.eEcActiveIdc = ERROR_CON_SLICE_MV_COPY_CROSS_IDR_FREEZE_RES_CHANGE;
  param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
  param.bParseOnly = false;
  if (decoder_->Initialize(&param) != cmResultSuccess) {
    close();
    return false;
  }
  return true;
}

void H264Decoder::close() {
  if (decoder_) {
    decoder_->Uninitialize();
    WelsDestroyDecoder(decoder_);
    decoder_ = nullptr;
  }
}

bool H264Decoder::decode(const uint8_t* data, size_t size, I420Buffer& out,
                         int* yStride, int* uvStride) {
  if (!decoder_ || !data || size == 0) {
    return false;
  }
  uint8_t* pData[3] = {nullptr, nullptr, nullptr};
  SBufferInfo bufInfo;
  memset(&bufInfo, 0, sizeof(bufInfo));

  DECODING_STATE state = decoder_->DecodeFrameNoDelay(
      data, static_cast<int>(size), pData, &bufInfo);
  (void)state;  // recoverable bitstream errors still yield a frame below
  if (!bufInfo.iBufferStatus || !pData[0]) {
    return false;
  }

  const int w = bufInfo.UsrData.sSystemBuffer.iWidth;
  const int h = bufInfo.UsrData.sSystemBuffer.iHeight;
  const int strideY = bufInfo.UsrData.sSystemBuffer.iStride[0];
  const int strideUV = bufInfo.UsrData.sSystemBuffer.iStride[1];
  if (w <= 0 || h <= 0) {
    return false;
  }

  // Copy out of the decoder's internal buffer.
  out.width = w;
  out.height = h;
  out.y.resize(static_cast<size_t>(strideY) * h);
  out.u.resize(static_cast<size_t>(strideUV) * ((h + 1) / 2));
  out.v.resize(static_cast<size_t>(strideUV) * ((h + 1) / 2));
  for (int r = 0; r < h; ++r) {
    memcpy(out.y.data() + static_cast<size_t>(r) * strideY,
           pData[0] + static_cast<size_t>(r) * strideY,
           static_cast<size_t>(w));
  }
  for (int r = 0; r < (h + 1) / 2; ++r) {
    memcpy(out.u.data() + static_cast<size_t>(r) * strideUV,
           pData[1] + static_cast<size_t>(r) * strideUV,
           static_cast<size_t>((w + 1) / 2));
    memcpy(out.v.data() + static_cast<size_t>(r) * strideUV,
           pData[2] + static_cast<size_t>(r) * strideUV,
           static_cast<size_t>((w + 1) / 2));
  }
  if (yStride) {
    *yStride = strideY;
  }
  if (uvStride) {
    *uvStride = strideUV;
  }
  return true;
}

}  // namespace rdc
