// remote_desktop — DataChannel wrapper for JSON input/meta messages.
#ifndef REMOTE_DESKTOP_NET_CONTROL_CHANNEL_H_
#define REMOTE_DESKTOP_NET_CONTROL_CHANNEL_H_

#include <atomic>
#include <memory>
#include <mutex>

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <rtc/rtc.hpp>

#include "core/input_event.h"

namespace rd {

class ControlChannel : public QObject {
  Q_OBJECT
 public:
  explicit ControlChannel(QObject* parent = nullptr);
  ~ControlChannel() override;

  // Viewer role: create the channel (negotiated later with the offer).
  void createOn(rtc::PeerConnection& pc);

  // Host role: accept the remote-created channel.
  void acceptOn(rtc::PeerConnection& pc);

  void send(const QJsonObject& obj);
  void sendInput(const InputEvent& ev);
  void sendFrameMeta(int width, int height);
  // Viewer → host per-second video stats (deltas over the last second).
  void sendVideoStats(int received, int decoded, int decodeFailed);
  // Viewer → host quality mode (0=auto, 1=native, 2=high, 3=smooth).
  void sendQualityMode(int mode);

  bool isOpen() const;

 signals:
  void opened();
  void closed();
  void inputReceived(const rd::InputEvent& ev);
  void frameMetaReceived(int width, int height);
  void videoStatsReceived(int received, int decoded, int decodeFailed);
  void qualityModeSelected(int mode);
  void errorText(const QString& text);

 private:
  void wire(rtc::DataChannel& dc);

  // dc_ is assigned on a libdatachannel thread (onDataChannel) while
  // send()/isOpen() run on the UI thread.
  mutable std::mutex mtx_;
  std::shared_ptr<rtc::DataChannel> dc_;
  // Held by every callback; set to false in the destructor so late callbacks
  // bail out instead of touching a dead object.
  std::shared_ptr<std::atomic_bool> alive_;
};

}  // namespace rd

#endif
