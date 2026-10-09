// remote_desktop — Qt wrapper around rtc::PeerConnection.
//
// Non-trickle ICE: localDescription() only returns once gathering is
// complete, so the whole SDP can be copy-pasted. Video track direction and
// DataChannel are set up by RemoteDesktopSession through this class.
#ifndef REMOTE_DESKTOP_NET_PEER_SESSION_H_
#define REMOTE_DESKTOP_NET_PEER_SESSION_H_

#include <functional>

#include <QObject>
#include <QString>

#include <rtc/rtc.hpp>

namespace rd {

class PeerSession : public QObject {
  Q_OBJECT
 public:
  explicit PeerSession(QObject* parent = nullptr);
  ~PeerSession() override;

  // Must be called once before anything else. `isOfferer` decides the
  // gathering/copy flow; a video transceiver is added in the requested
  // direction (sendonly for host, recvonly for viewer).
  bool start(bool isOfferer, bool sendVideo);

  // Offerer side: builds the local offer (blocks until ICE gathering
  // completes) and returns the full SDP text.
  QString createOffer();

  // Answerer side: accepts the remote offer, builds the local answer and
  // returns it once gathering is complete.
  QString acceptOfferAndCreateAnswer(const QString& offerSdp);

  // Offerer side: applies the pasted answer.
  bool acceptAnswer(const QString& answerSdp);

  void close();

  rtc::PeerConnection& pc() { return *pc_; }
  bool isOfferer() const { return offerer_; }

  // Track added by US via addTrack (offerer only, RecvOnly for the viewer).
  // The answerer gets its outgoing track through onTrack instead — with a
  // pre-added track on the answerer, libdatachannel matches the offer's
  // m-line mid against it and onTrack never fires.
  std::shared_ptr<rtc::Track> localTrack() const { return localTrack_; }

 signals:
  void connected();
  void disconnected();
  void failed(const QString& reason);
  void stateChanged(const QString& text);

 private:
  QString localDescriptionBlocking();

  std::optional<rtc::PeerConnection> pc_;
  rtc::Configuration config_;
  std::shared_ptr<rtc::Track> localTrack_;
  bool offerer_ = false;
  bool started_ = false;
};

}  // namespace rd

#endif
