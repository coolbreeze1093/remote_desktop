#include "net/peer_session.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QThread>

#include "rtc/rtc.hpp"

#include "../debug_log.h"

namespace rd {

namespace {
rtc::Description::Video makeVideoMedia(const std::string& mid) {
  rtc::Description::Video media(mid, rtc::Description::Direction::SendOnly);
  media.addH264Codec(96);
  return media;
}
}  // namespace

PeerSession::PeerSession(QObject* parent) : QObject(parent) {
  // STUN is only useful across NATs; it also produces srflx candidates whose
  // NAT-hairpin path can be selected and then silently drop everything on
  // same-machine/LAN sessions. RD_NO_STUN=1 disables it (diagnostics).
  if (qEnvironmentVariableIsEmpty("RD_NO_STUN")) {
    config_.iceServers.emplace_back("stun:stun.l.google.com:19302");
  }
}

PeerSession::~PeerSession() { close(); }

bool PeerSession::start(bool isOfferer, bool sendVideo) {
  if (started_) {
    return false;
  }
  offerer_ = isOfferer;
  started_ = true;

  pc_.emplace(config_);

  pc_->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
    if (state == rtc::PeerConnection::GatheringState::Complete) {
      rdDebug("pc", "gathering complete");
      QMetaObject::invokeMethod(this, "stateChanged", Qt::QueuedConnection,
                                Q_ARG(QString, QStringLiteral("ICE 收集完成")));
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
      case rtc::PeerConnection::IceState::Disconnected: text = "disconnected"; break;
      case rtc::PeerConnection::IceState::Closed: text = "closed"; break;
    }
    rdDebug("pc", "ice=%s", text);
  });

  pc_->onStateChange([this](rtc::PeerConnection::State state) {
    QString text;
    switch (state) {
      case rtc::PeerConnection::State::New: text = "new"; break;
      case rtc::PeerConnection::State::Connecting: text = "connecting"; break;
      case rtc::PeerConnection::State::Connected:
        QMetaObject::invokeMethod(this, "connected", Qt::QueuedConnection);
        text = "connected";
        break;
      case rtc::PeerConnection::State::Disconnected:
        QMetaObject::invokeMethod(this, "disconnected", Qt::QueuedConnection);
        text = "disconnected";
        break;
      case rtc::PeerConnection::State::Failed:
        QMetaObject::invokeMethod(this, "failed", Qt::QueuedConnection,
                                  Q_ARG(QString, QStringLiteral("连接失败")));
        text = "failed";
        break;
      case rtc::PeerConnection::State::Closed: text = "closed"; break;
    }
    rdDebug("pc", "state=%s", text.toUtf8().constData());
    QMetaObject::invokeMethod(this, "stateChanged", Qt::QueuedConnection,
                              Q_ARG(QString, text));
  });

  // Video transceiver direction:
  //  - Offerer (viewer) MUST add a RecvOnly video track so its offer carries
  //    a video m-line the host can reciprocate. Keep the returned Track: the
  //    pre-added track is the one that receives the stream (onTrack does not
  //    fire for a mid that already has a local track).
  //  - Answerer (host) adds NO track: setRemoteDescription(offer) creates the
  //    reciprocated SendOnly track from the offer's m-line and fires onTrack.
  if (isOfferer) {
    rtc::Description::Video media("video0", sendVideo
                                                ? rtc::Description::Direction::SendOnly
                                                : rtc::Description::Direction::RecvOnly);
    media.addH264Codec(96);
    localTrack_ = pc_->addTrack(media);
  }

  return true;
}

QString PeerSession::createOffer() {
  if (!pc_) {
    return {};
  }
  pc_->setLocalDescription();
  const QString sdp = localDescriptionBlocking();
  return sdp;
}

QString PeerSession::acceptOfferAndCreateAnswer(const QString& offerSdp) {
  if (!pc_) {
    return {};
  }
  try {
    rtc::Description desc(offerSdp.toStdString(), "offer");
    pc_->setRemoteDescription(desc);
  } catch (const std::exception& e) {
    QMetaObject::invokeMethod(this, "failed", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromUtf8(e.what())));
    return {};
  }
  // Do NOT call setLocalDescription() here: with auto-negotiation enabled
  // (the default), setRemoteDescription(offer) already generated and set the
  // answer. A second explicit call now runs in the Stable state and produces
  // an OFFER (setup:actpass) that replaces the answer — the viewer then
  // rejects it ("Illegal role actpass in remote answer").
  return localDescriptionBlocking();
}

bool PeerSession::acceptAnswer(const QString& answerSdp) {
  if (!pc_) {
    return false;
  }
  try {
    rtc::Description desc(answerSdp.toStdString(), "answer");
    pc_->setRemoteDescription(desc);
  } catch (const std::exception& e) {
    QMetaObject::invokeMethod(this, "failed", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromUtf8(e.what())));
    return false;
  }
  return true;
}

void PeerSession::close() {
  if (pc_) {
    pc_->close();
    pc_.reset();
  }
  localTrack_.reset();
  started_ = false;
}

QString PeerSession::localDescriptionBlocking() {
  if (!pc_) {
    return {};
  }
  // Non-trickle: wait for gathering to complete, then hand out the full SDP.
  // Bounded: without network access gathering never completes and this would
  // spin the UI thread forever.
  constexpr int kMaxWaitMs = 10000;
  int waited = 0;
  while (pc_->gatheringState() !=
         rtc::PeerConnection::GatheringState::Complete) {
    QThread::msleep(50);
    waited += 50;
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    if (waited >= kMaxWaitMs) {
      QMetaObject::invokeMethod(this, "failed", Qt::QueuedConnection,
                                Q_ARG(QString, QStringLiteral("ICE 收集超时，请检查网络")));
      return {};
    }
  }
  auto desc = pc_->localDescription();
  return QString::fromStdString(std::string(desc.value()));
}

}  // namespace rd
