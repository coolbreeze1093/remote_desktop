// remote_desktop — orchestrates capture/encode/send (host) and receive/
// decode (viewer) on top of PeerSession, ControlChannel and the video layer.
#ifndef REMOTE_DESKTOP_SESSION_REMOTE_DESKTOP_SESSION_H_
#define REMOTE_DESKTOP_SESSION_REMOTE_DESKTOP_SESSION_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <QImage>
#include <QObject>

#include <rtc/rtc.hpp>

#include "core/i420_frame.h"
#include "core/input_event.h"
#include "core/input_injector.h"
#include "core/openh264_codec.h"
#include "net/control_channel.h"
#include "net/h264_stream_sender.h"
#include "net/peer_session.h"
#include "session/video_quality.h"

class QTimer;

namespace rd {

class ScreenCaptureThread;
struct SelfTestHooks;  // test-only backdoor, defined in self_test.cc

class RemoteDesktopSession : public QObject {
  Q_OBJECT
  friend struct SelfTestHooks;
 public:
  enum class Role { None, Host, Viewer };

  // Pipeline counters (the UI polls these to localise "no picture" faults:
  // capture → encode → send → receive → decode → display).
  struct DiagStats {
    uint32_t captured = 0;      // host: frames pulled from screen_capture_lib
    uint32_t encoded = 0;       // host: non-empty encoder output
    uint32_t sent = 0;          // host: access units handed to the track
    uint32_t dropped = 0;       // host: frames dropped, encode queue was full
    uint32_t received = 0;      // viewer: depacketized frames from the track
    uint32_t decoded = 0;       // viewer: frames that produced a QImage
    uint32_t decodeFailed = 0;  // viewer: frames OpenH264 could not decode
    int qualityLevel = -1;      // host: active ladder level (0..2)
    int encodeWidth = 0;        // host: current encode size
    int encodeHeight = 0;
    int encodeMs = 0;           // host: EMA per-frame pipeline cost
  };

  explicit RemoteDesktopSession(QObject* parent = nullptr);
  ~RemoteDesktopSession() override;

  // --- Host (controlled machine) ---
  bool startHost(int64_t screenId);
  QString hostAcceptOffer(const QString& offerSdp);  // returns answer SDP
  // Convenience for the UI: startHost + apply the offer; on any failure the
  // session is reset (stop()) so the user can retry, and {} is returned.
  QString hostAcceptOfferChecked(const QString& offerSdp, int64_t screenId);

  // --- Viewer (controlling machine) ---
  bool startViewer();
  QString viewerCreateOffer();                   // returns offer SDP
  bool viewerAcceptAnswer(const QString& answerSdp);

  void stop();

  Role role() const { return role_; }
  PeerSession* peer() const { return peer_.get(); }
  ControlChannel* control() const { return control_; }
  // Quality mode: 0=auto (default), 1=超清, 2=高清, 3=流畅. The host applies
  // it on the capture thread; when the control channel is open the choice is
  // announced to the host immediately.
  void setVideoQualityMode(int mode);
  DiagStats diagStats() const {
    return {captured_.load(),
            encoded_.load(),
            sent_.load(),
            dropped_.load(),
            received_.load(),
            decoded_.load(),
            decodeFailed_.load(),
            quality_.level(),
            encodeW_.load(),
            encodeH_.load(),
            static_cast<int>(encodeEmaMs_.load())};
  }

 signals:
  void connected();
  void disconnected();
  void failed(const QString& reason);
  void statusText(const QString& text);
  void frameReady(const QImage& image);
  void inputInjected();

 private slots:
  void onInputReceived(InputEvent ev);

 private:
  friend class HostLoopSink;
  bool openCapture(int64_t screenId);
  void hostLoop();
  void stopThreads();
  void resetDiag();
  void sendVideoStatsTick();

  Role role_ = Role::None;

  // Set to false first thing in stop(): callbacks running on libdatachannel
  // threads check this (they hold a copy of the shared_ptr) before touching
  // session members.
  std::shared_ptr<std::atomic_bool> alive_ =
      std::make_shared<std::atomic_bool>(true);

  std::unique_ptr<PeerSession> peer_;
  ControlChannel* control_ = nullptr;

  // Host side
  std::unique_ptr<ScreenCaptureThread> capture_;
  std::unique_ptr<H264Encoder> encoder_;
  H264StreamSender sender_;
  std::shared_ptr<rtc::Track> sendTrack_;
  InputInjector injector_;
  std::unique_ptr<std::thread> hostThread_;
  std::atomic_bool hostRunning_{false};
  std::atomic_bool keyframeRequested_{true};
  int64_t screenId_ = 0;

  // Encode pipeline queue: the capture thread copies each BGRA frame in and
  // returns immediately; hostLoop() (the encoder thread) is the only
  // consumer. Bounded so latency cannot accumulate: when the encoder falls
  // behind, the OLDEST queued frame is dropped and the freshest one is kept.
  struct PendingFrame {
    std::vector<uint8_t> bgra;  // full BGRA copy (stride * height bytes)
    int width = 0;
    int height = 0;
    int stride = 0;
    int originX = 0;
    int originY = 0;
  };
  std::mutex encQueueMtx_;
  std::condition_variable encQueueCv_;
  std::deque<PendingFrame> encQueue_;

  // Diag counters (cross-thread: capture thread / rtc threads / UI).
  std::atomic<uint32_t> captured_{0}, encoded_{0}, sent_{0}, dropped_{0};
  std::atomic<uint32_t> received_{0}, decoded_{0}, decodeFailed_{0};

  // Dynamic video quality. qualityMode_ comes from the Viewer UI (its own or
  // via the control channel); stat* accumulate viewer per-second deltas set on
  // the UI thread and are drained by the encoder thread's evaluate() tick.
  // encodeW_/H_/EmaMs_ are diagnostics written by the encoder thread.
  VideoQualityController quality_;  // encoder thread (hostLoop) only
  std::atomic<int> qualityMode_{0};
  std::atomic<int> statRecv_{0}, statDec_{0}, statFail_{0};
  std::atomic<int> encodeW_{0}, encodeH_{0};
  std::atomic<float> encodeEmaMs_{0};
  QTimer* statsTimer_ = nullptr;  // viewer: per-second stats upload
  uint32_t lastSentRecv_ = 0, lastSentDec_ = 0, lastSentFail_ = 0;
  // Viewer stall detector (sendVideoStatsTick): counts ticks with no new
  // received frames while the connection is up — the "frozen remote" symptom.
  uint32_t stallLastRecv_ = 0;
  int stallTicks_ = 0;

  // Viewer side
  std::mutex decoderMtx_;  // decode runs on libdatachannel threads
  std::unique_ptr<H264Decoder> decoder_;
};

}  // namespace rd

#endif
