#include "net/h264_stream_sender.h"

#include <algorithm>

namespace rd {

void H264StreamSender::attach(std::shared_ptr<rtc::Track> track,
                              std::function<void()> onPli) {
  std::lock_guard<std::mutex> lock(mtx_);
  track_ = track;
  if (!track_) {
    return;
  }
  // SSRC/CNAME are arbitrary locally; the packetizer stamps RTP headers.
  auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
      /*ssrc=*/42, "remote-desktop", /*payloadType=*/96,
      /*clockRate=*/rtc::H264RtpPacketizer::ClockRate);
  packetizer_ = std::make_shared<rtc::H264RtpPacketizer>(
      rtc::NalUnit::Separator::LongStartSequence, rtpConfig);
  // NACK retransmission: UDP loss on the IDR would otherwise leave the viewer
  // black until the next periodic keyframe.
  packetizer_->addToChain(std::make_shared<rtc::RtcpNackResponder>());
  // Answer PLI/FIR keyframe requests from the receiver.
  if (onPli) {
    packetizer_->addToChain(std::make_shared<rtc::PliHandler>(std::move(onPli)));
  }
  track_->setMediaHandler(packetizer_);
  startTime_ = std::chrono::steady_clock::now();
  wasOpen_ = false;
  openTransition_ = false;
}

void H264StreamSender::detach() {
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

bool H264StreamSender::attached() const {
  std::lock_guard<std::mutex> lock(mtx_);
  return static_cast<bool>(track_);
}

bool H264StreamSender::consumeOpenTransition() {
  std::lock_guard<std::mutex> lock(mtx_);
  if (openTransition_) {
    openTransition_ = false;
    return true;
  }
  return false;
}

void H264StreamSender::send(const std::vector<uint8_t>& annexB, bool keyframe) {
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
      openTransition_ = true;  // picked up by the next onFrame
    }
    wasOpen_ = open;
    if (!open) {
      return;
    }
    if (!loggedOpen_) {
      loggedOpen_ = true;
      fprintf(stderr, "[sender] first send on open track\n");
      fflush(stderr);
    }
    // Keep the objects alive for the duration of the send even if detach()
    // runs concurrently.
    track = track_;
    packetizer = packetizer_;
  }
  // Advance the 90 kHz timestamp from wall clock, then hand the raw Annex-B
  // access unit to the track: Track::send runs the media-handler chain, so
  // the packetizer fragments it into RTP packets and transports them.
  // (Calling packetizer_->outgoing() manually here would packetize twice.)
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
    fprintf(stderr, "[sender] send #%d size=%zu ret=%d\n", ++sentCount_,
            annexB.size(), (int)sent);
    fflush(stderr);
  } else {
    ++sentCount_;
  }
}

uint32_t H264StreamSender::rtpTimestamp() const {
  std::lock_guard<std::mutex> lock(mtx_);
  if (!packetizer_) {
    return 0;
  }
  return packetizer_->rtpConfig->timestamp;
}

}  // namespace rd
