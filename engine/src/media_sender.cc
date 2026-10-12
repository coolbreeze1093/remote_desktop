// rdc — 把 H.264 Annex-B access unit 经 rtc::Track 发送（H264RtpPacketizer，
// 一次一个 AU，90kHz 时钟）。自旧 net/h264_stream_sender.cc 原样迁移。
#include "media_sender.h"

#include <algorithm>

#include "rdc/log.h"

namespace rdc {

void MediaSender::attach(std::shared_ptr<rtc::Track> track,
                         std::function<void()> onPli) {
  std::lock_guard<std::mutex> lock(mtx_);
  track_ = track;
  if (!track_) {
    return;
  }
  // SSRC/CNAME 本地随意；RTP 头由打包器盖章。
  auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
      /*ssrc=*/42, "remote-desktop", /*payloadType=*/96,
      /*clockRate=*/rtc::H264RtpPacketizer::ClockRate);
  packetizer_ = std::make_shared<rtc::H264RtpPacketizer>(
      rtc::NalUnit::Separator::LongStartSequence, rtpConfig);
  // NACK 重传：IDR 的 UDP 丢包否则会让 Viewer 黑屏到下一个周期关键帧。
  packetizer_->addToChain(std::make_shared<rtc::RtcpNackResponder>());
  // 应答接收方的 PLI/FIR 关键帧请求。
  if (onPli) {
    packetizer_->addToChain(
        std::make_shared<rtc::PliHandler>(std::move(onPli)));
  }
  track_->setMediaHandler(packetizer_);
  startTime_ = std::chrono::steady_clock::now();
  wasOpen_ = false;
  openTransition_ = false;
}

void MediaSender::detach() {
  std::shared_ptr<rtc::Track> track;
  {
    std::lock_guard<std::mutex> lock(mtx_);
    track = track_;
    track_.reset();
    packetizer_.reset();
    wasOpen_ = false;
    openTransition_ = false;
  }
  if (track) {
    track->setMediaHandler(nullptr);
  }
}

bool MediaSender::attached() const {
  std::lock_guard<std::mutex> lock(mtx_);
  return static_cast<bool>(track_);
}

bool MediaSender::consumeOpenTransition() {
  std::lock_guard<std::mutex> lock(mtx_);
  if (openTransition_) {
    openTransition_ = false;
    return true;
  }
  return false;
}

void MediaSender::send(const std::vector<uint8_t>& annexB, bool keyframe) {
  (void)keyframe;
  std::shared_ptr<rtc::Track> track;
  std::shared_ptr<rtc::H264RtpPacketizer> packetizer;
  {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!track_ || !packetizer_ || annexB.empty()) {
      return;
    }
    const bool open = track_->isOpen();
    if (open && !wasOpen_) {
      openTransition_ = true;  // 下一次 onFrame 处理
    }
    wasOpen_ = open;
    if (!open) {
      return;
    }
    if (!loggedOpen_) {
      loggedOpen_ = true;
      rdcDebug("sender", "first send on open track");
    }
    // 即使 detach() 并发发生，也要让对象活过这次 send。
    track = track_;
    packetizer = packetizer_;
  }
  // 90kHz 时间戳按墙钟推进，然后把裸 Annex-B AU 交给 track：Track::send 会
  // 跑媒体处理器链，打包器拆成 RTP 包并送传输。
  // （手动调 packetizer_->outgoing() 会打包两次。）
  const auto now = std::chrono::steady_clock::now();
  const double elapsedSec =
      std::chrono::duration<double>(now - startTime_).count();
  auto rtpConfig = packetizer->rtpConfig;
  rtpConfig->timestamp =
      rtpConfig->startTimestamp + rtpConfig->secondsToTimestamp(elapsedSec);

  rtc::binary payload(reinterpret_cast<const std::byte*>(annexB.data()),
                      reinterpret_cast<const std::byte*>(annexB.data() +
                                                         annexB.size()));
  const bool sent = track->send(std::move(payload));
  if (!sent || sentCount_ < 3) {
    rdcDebug("sender", "send #%d size=%zu ret=%d", ++sentCount_, annexB.size(),
             (int)sent);
  } else {
    ++sentCount_;
  }
}

uint32_t MediaSender::rtpTimestamp() const {
  std::lock_guard<std::mutex> lock(mtx_);
  if (!packetizer_) {
    return 0;
  }
  return packetizer_->rtpConfig->timestamp;
}

}  // namespace rdc
