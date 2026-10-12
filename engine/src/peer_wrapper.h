// rdc — rtc::PeerConnection 封装：建连、SDP 三步（offer/answer/apply）、
// 阻塞式 ICE 收集等待、状态回调。
#ifndef RDC_SRC_PEER_WRAPPER_H_
#define RDC_SRC_PEER_WRAPPER_H_

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rtc/rtc.hpp>

namespace rdc {

class PeerWrapper {
 public:
  using StateFn = std::function<void(rtc::PeerConnection::State)>;
  using TrackFn = std::function<void(std::shared_ptr<rtc::Track>)>;

  ~PeerWrapper();

  // 建一次 PeerConnection。isOfferer 决定收集/交换流程；offerer 且 sendVideo
  // 为 false 时加 RecvOnly 收流轨道（viewer 的角色）。
  bool Start(bool isOfferer, bool sendVideo,
             const std::vector<std::string>& iceServers, StateFn onState,
             TrackFn onTrack);

  // Offerer：设本地描述并阻塞等收集完成，返回完整 SDP（失败返回空 +
  // *error）。
  std::string CreateOfferBlocking(std::string* error);

  // Answerer：应用远端 offer，等收集完成返回 answer SDP。
  // 注意：不显式 setLocalDescription——自动协商下 setRemoteDescription(offer)
  // 已生成 answer（原因见实现内注释）。
  std::string AcceptOfferCreateAnswerBlocking(const std::string& offerSdp,
                                              std::string* error);

  // Offerer：应用远端 answer。
  bool AcceptAnswer(const std::string& answerSdp, std::string* error);

  // Viewer 预加的收流轨道（对既有 mid 不触发 onTrack，必须从这拿）。
  std::shared_ptr<rtc::Track> localTrack() const { return localTrack_; }

  // 供控制通道挂接（createDataChannel / onDataChannel）。
  rtc::PeerConnection& connection() { return *pc_; }

  void Close();

 private:
  bool WaitGathering(std::string* error);
  std::string LocalDescription() const;
  static void SetError(std::string* error, const char* msg);

  std::optional<rtc::PeerConnection> pc_;
  std::shared_ptr<rtc::Track> localTrack_;
  bool offerer_ = false;
  bool started_ = false;

  std::mutex gatherMtx_;
  std::condition_variable gatherCv_;
  bool gathered_ = false;
};

}  // namespace rdc
#endif  // RDC_SRC_PEER_WRAPPER_H_
