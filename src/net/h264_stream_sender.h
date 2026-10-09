// remote_desktop — sends an H.264 Annex-B stream over an rtc::Track using the
// H264RtpPacketizer (one access unit per packet burst, 90 kHz clock).
#ifndef REMOTE_DESKTOP_NET_H264_STREAM_SENDER_H_
#define REMOTE_DESKTOP_NET_H264_STREAM_SENDER_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include <rtc/rtc.hpp>

namespace rd {

// Thread-safe: attach()/detach() run on the UI thread while send() is called
// from the capture thread.
class H264StreamSender {
 public:
  // Takes ownership of the track's packetization. `onPli` fires on a
  // libdatachannel thread when the receiver requests a keyframe (RTCP PLI/FIR).
  void attach(std::shared_ptr<rtc::Track> track,
              std::function<void()> onPli = nullptr);

  void detach();

  bool attached() const;

  // True exactly once each time the attached track transitions to open. The
  // caller must force an IDR then: a decoder joining mid-GOP has nothing to
  // start from until the next keyframe.
  bool consumeOpenTransition();

  // Sends one Annex-B access unit. `keyframe` only affects initial timing
  // bookkeeping; RTP payload is identical.
  void send(const std::vector<uint8_t>& annexB, bool keyframe);

  uint32_t rtpTimestamp() const;

 private:
  mutable std::mutex mtx_;
  std::shared_ptr<rtc::Track> track_;
  std::shared_ptr<rtc::H264RtpPacketizer> packetizer_;
  std::chrono::steady_clock::time_point startTime_ =
      std::chrono::steady_clock::now();
  bool wasOpen_ = false;
  bool openTransition_ = false;
  bool loggedOpen_ = false;  // one-shot debug log
  int sentCount_ = 0;        // debug counter
};

}  // namespace rd

#endif
