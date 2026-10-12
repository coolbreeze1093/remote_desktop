// rdc — rtc::PeerConnection 封装（纯 C++，无 Qt）。非 trickle：SDP 阻塞到
// ICE 收集完成（条件变量等待，上限 10s），整段 SDP 手动交换。
#include "peer_wrapper.h"

#include <condition_variable>

#include "rdc/log.h"

namespace rdc {

namespace {
rtc::Description::Video makeVideoMedia(const std::string& mid,
                                       rtc::Description::Direction dir) {
  rtc::Description::Video media(mid, dir);
  media.addH264Codec(96);
  return media;
}
}  // namespace

PeerWrapper::~PeerWrapper() { Close(); }

bool PeerWrapper::Start(bool isOfferer, bool sendVideo,
                        const std::vector<std::string>& iceServers,
                        StateFn onState, TrackFn onTrack) {
  if (started_) {
    return false;
  }
  offerer_ = isOfferer;
  started_ = true;

  rtc::Configuration config;
  // rtc::IceServer 可从 string 隐式构造（旧 PeerSession 同款用法）。
  for (const std::string& url : iceServers) {
    config.iceServers.emplace_back(url);
  }
  pc_.emplace(config);

  pc_->onGatheringStateChange(
      [this](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
          rdcDebug("pc", "gathering complete");
          {
            std::lock_guard<std::mutex> lock(gatherMtx_);
            gathered_ = true;
          }
          gatherCv_.notify_all();
        }
      });

  pc_->onIceStateChange([this](rtc::PeerConnection::IceState state) {
    const char* text = "?";
    switch (state) {
      case rtc::PeerConnection::IceState::New: text = "new"; break;
      case rtc::PeerConnection::IceState::Checking: text = "checking"; break;
      case rtc::PeerConnection::IceState::Connected: text = "connected"; break;
      case rtc::PeerConnection::IceState::Completed: text = "completed"; break;
      case rtc::PeerConnection::IceState::Failed: text = "FAILED"; break;
      case rtc::PeerConnection::IceState::Disconnected:
        text = "disconnected";
        break;
      case rtc::PeerConnection::IceState::Closed: text = "closed"; break;
    }
    rdcDebug("pc", "ice=%s", text);
  });

  pc_->onStateChange([this, onState](rtc::PeerConnection::State state) {
    const char* text = "?";
    switch (state) {
      case rtc::PeerConnection::State::New: text = "new"; break;
      case rtc::PeerConnection::State::Connecting: text = "connecting"; break;
      case rtc::PeerConnection::State::Connected: text = "connected"; break;
      case rtc::PeerConnection::State::Disconnected:
        text = "disconnected";
        break;
      case rtc::PeerConnection::State::Failed: text = "failed"; break;
      case rtc::PeerConnection::State::Closed: text = "closed"; break;
    }
    rdcDebug("pc", "state=%s", text);
    if (onState) {
      onState(state);
    }
  });

  // 视频收发方向：
  //  - Offerer（viewer）必须加一条 RecvOnly 视频轨道，offer 才带视频 m-line
  //    供 host 回应。保留返回的 Track：预加的轨道就是收流轨道（对已有本地
  //    轨道的 mid，libdatachannel 不再触发 onTrack）。
  //  - Answerer（host）不加轨道：setRemoteDescription(offer) 会从 offer 的
  //    m-line 派生出 SendOnly 轨道并触发 onTrack。
  if (isOfferer) {
    localTrack_ = pc_->addTrack(makeVideoMedia(
        "video0", sendVideo ? rtc::Description::Direction::SendOnly
                            : rtc::Description::Direction::RecvOnly));
  }
  if (onTrack) {
    pc_->onTrack(std::move(onTrack));
  }
  return true;
}

std::string PeerWrapper::CreateOfferBlocking(std::string* error) {
  if (!pc_) {
    SetError(error, "peer connection not started");
    return {};
  }
  pc_->setLocalDescription();
  if (!WaitGathering(error)) {
    return {};
  }
  return LocalDescription();
}

std::string PeerWrapper::AcceptOfferCreateAnswerBlocking(const std::string& offerSdp,
                                                         std::string* error) {
  if (!pc_) {
    SetError(error, "peer connection not started");
    return {};
  }
  try {
    rtc::Description desc(offerSdp, "offer");
    pc_->setRemoteDescription(desc);
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return {};
  }
  // 此处不要调 setLocalDescription()：自动协商（默认开）下
  // setRemoteDescription(offer) 已经生成并设置了 answer。此时再显式调用会在
  // Stable 态产出一个 OFFER（setup:actpass）顶掉 answer，viewer 会报
  // "Illegal role actpass in remote answer"。
  if (!WaitGathering(error)) {
    return {};
  }
  return LocalDescription();
}

bool PeerWrapper::AcceptAnswer(const std::string& answerSdp,
                               std::string* error) {
  if (!pc_) {
    SetError(error, "peer connection not started");
    return false;
  }
  try {
    rtc::Description desc(answerSdp, "answer");
    pc_->setRemoteDescription(desc);
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return false;
  }
  return true;
}

void PeerWrapper::Close() {
  if (pc_) {
    pc_->close();
    pc_.reset();
  }
  localTrack_.reset();
  started_ = false;
  {
    std::lock_guard<std::mutex> lock(gatherMtx_);
    gathered_ = false;
  }
}

bool PeerWrapper::WaitGathering(std::string* error) {
  if (!pc_) {
    SetError(error, "peer connection closed");
    return false;
  }
  // 非 trickle：等收集完成后给出整段 SDP。有界等待：无网络时收集永远不会
  // 完成，不能无限等。
  std::unique_lock<std::mutex> lock(gatherMtx_);
  constexpr auto kMaxWait = std::chrono::seconds(10);
  const bool ok = gatherCv_.wait_for(lock, kMaxWait, [this] {
    return gathered_ || !pc_.has_value();
  });
  if (!ok) {
    SetError(error, "ICE gathering timeout, check network");
    return false;
  }
  return pc_.has_value();
}

std::string PeerWrapper::LocalDescription() const {
  if (!pc_) {
    return {};
  }
  auto desc = pc_->localDescription();
  if (!desc.has_value()) {
    return {};
  }
  return std::string(desc.value());
}

void PeerWrapper::SetError(std::string* error, const char* msg) {
  rdcDebug("pc", "error: %s", msg);
  if (error) {
    *error = msg;
  }
}

}  // namespace rdc
