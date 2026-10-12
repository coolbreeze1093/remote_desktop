// rdc — rtc::Track 的 H.264 Annex-B 发送封装。attach/detach 在信令线程，
// send 在编码线程，互斥保护。
#ifndef RDC_SRC_MEDIA_SENDER_H_
#define RDC_SRC_MEDIA_SENDER_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include <rtc/rtc.hpp>

namespace rdc {

class MediaSender {
 public:
  // 接管 track 的打包链。`onPli` 在 libdatachannel 线程触发（接收方请求
  // 关键帧，RTCP PLI/FIR）。
  void attach(std::shared_ptr<rtc::Track> track,
              std::function<void()> onPli = nullptr);

  void detach();

  bool attached() const;

  // 每次 attached track 转为 open 时恰好返回一次 true：解码端中途入会没有
  // 起始参考帧，必须强制一个 IDR。
  bool consumeOpenTransition();

  // 发送一个 Annex-B access unit。keyframe 只影响时序簿记，RTP 载荷相同。
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
  bool loggedOpen_ = false;  // 一次性调试日志
  int sentCount_ = 0;        // 调试计数
};

}  // namespace rdc
#endif  // RDC_SRC_MEDIA_SENDER_H_
