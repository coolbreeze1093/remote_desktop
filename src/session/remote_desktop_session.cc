#include "session/remote_desktop_session.h"

#include <QMetaObject>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstring>

#include "debug_log.h"
#include "session/screen_capture_thread.h"

namespace rd {

namespace {
constexpr int kCaptureFps = 24;
// Placeholder until the first capture geometry arrives; the quality ladder
// reconfigures to the real level within the first second.
constexpr int kInitialEncodeWidth = 1280;
constexpr int kInitialEncodeHeight = 720;
constexpr int kInitialBitrateKbps = 4000;
// Encode queue capacity (plus one frame being encoded): enough slack to ride
// out an IDR/reconfigure spike without dropping, while capping pipeline
// latency at ~3 frame intervals and BGRA memory at a few frame buffers.
constexpr size_t kEncQueueCapacity = 3;
}  // namespace

RemoteDesktopSession::RemoteDesktopSession(QObject* parent)
    : QObject(parent), control_(new ControlChannel(this)) {
  // InputEvent crosses threads via queued invokeMethod; without registration
  // Qt silently drops every delivery and the host never injects input.
  qRegisterMetaType<rd::InputEvent>("rd::InputEvent");
  connect(control_, &ControlChannel::inputReceived, this,
          &RemoteDesktopSession::onInputReceived);
  // Viewer per-second stats feed the host-side quality ladder (deltas are
  // accumulated here and drained by the capture thread's evaluate() tick).
  connect(control_, &ControlChannel::videoStatsReceived, this,
          [this](int recv, int dec, int fail) {
            if (role_ != Role::Host) {
              return;
            }
            statRecv_.fetch_add(recv, std::memory_order_relaxed);
            statDec_.fetch_add(dec, std::memory_order_relaxed);
            statFail_.fetch_add(fail, std::memory_order_relaxed);
          });
  // Manual quality picked on the Viewer UI.
  connect(control_, &ControlChannel::qualityModeSelected, this,
          [this](int mode) {
            if (role_ == Role::Host) {
              setVideoQualityMode(mode);
            }
          });
  // Let the viewer show the host's current encode size in its diagnostics.
  connect(control_, &ControlChannel::frameMetaReceived, this,
          [this](int w, int h) {
            encodeW_.store(w, std::memory_order_relaxed);
            encodeH_.store(h, std::memory_order_relaxed);
          });
}

RemoteDesktopSession::~RemoteDesktopSession() { stop(); }

bool RemoteDesktopSession::openCapture(int64_t screenId) {
  scl::Options opt;
  auto test = scl::Capturer::Create(SCL_SOURCE_SCREEN, opt);
  if (!test) {
    return false;
  }
  const bool ok = test->Select(screenId);
  screenId_ = screenId;
  return ok;
}

void RemoteDesktopSession::resetDiag() {
  captured_ = 0;
  encoded_ = 0;
  sent_ = 0;
  dropped_ = 0;
  received_ = 0;
  decoded_ = 0;
  decodeFailed_ = 0;
}

void RemoteDesktopSession::setVideoQualityMode(int mode) {
  qualityMode_.store(mode, std::memory_order_relaxed);
  rdDebug("quality", "mode set to %d (role=%s)", mode,
          role_ == Role::Host ? "host" : "viewer");
  if (role_ == Role::Viewer && control_ && control_->isOpen()) {
    control_->sendQualityMode(mode);
  }
}

void RemoteDesktopSession::sendVideoStatsTick() {
  if (role_ != Role::Viewer || !control_ || !control_->isOpen()) {
    return;
  }
  const uint32_t r = received_.load(std::memory_order_relaxed);
  const uint32_t d = decoded_.load(std::memory_order_relaxed);
  const uint32_t f = decodeFailed_.load(std::memory_order_relaxed);
  control_->sendVideoStats(static_cast<int>(r - lastSentRecv_),
                           static_cast<int>(d - lastSentDec_),
                           static_cast<int>(f - lastSentFail_));
  lastSentRecv_ = r;
  lastSentDec_ = d;
  lastSentFail_ = f;
  // Stall detector: while connected, received must keep advancing. When it
  // stops, the picture freezes with nothing in the log to say why — surface
  // the stall itself so a user report pinpoints a dead receive path.
  if (r != stallLastRecv_) {
    stallLastRecv_ = r;
    stallTicks_ = 0;
  } else if (++stallTicks_ == 3) {
    rdDebug("session",
            "viewer STALL: received static at %u for 3s (decoded=%u failed=%u)",
            r, d, f);
  }
}

bool RemoteDesktopSession::startHost(int64_t screenId) {
  if (role_ != Role::None || !openCapture(screenId)) {
    return false;
  }
  role_ = Role::Host;
  resetDiag();
  if (!qEnvironmentVariableIsEmpty("RD_TEST_NO_PEER_SESSION")) {
    // Test mode: run capture/encode without a peer connection.
    encoder_ = std::make_unique<H264Encoder>();
    if (!encoder_->init(kInitialEncodeWidth, kInitialEncodeHeight, kCaptureFps,
                        kInitialBitrateKbps)) {
      stop();
      emit failed(QStringLiteral("编码器初始化失败"));
      return false;
    }
    keyframeRequested_ = true;
    hostRunning_ = true;
    hostThread_ = std::make_unique<std::thread>([this] { hostLoop(); });
    return true;
  }

  peer_ = std::make_unique<PeerSession>();
  peer_->start(/*isOfferer=*/false, /*sendVideo=*/false);
  control_->acceptOn(peer_->pc());

  // The outgoing track arrives via onTrack while the offer is applied (it may
  // fire on a libdatachannel thread, even after this function returned). All
  // captures are by value; the attach itself hops to our thread so track
  // state is only ever touched here.
  const auto alive = alive_;
  peer_->pc().onTrack([this, alive](std::shared_ptr<rtc::Track> track) {
    fprintf(stderr, "[host] onTrack video=%d\n",
            (int)(track->description().type() == "video"));
    fflush(stderr);
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    if (track->description().type() != "video") {
      return;
    }
    QMetaObject::invokeMethod(
        this,
        [this, track] {
          if (!peer_) {
            return;
          }
          const char* dir = "?";
          switch (track->description().direction()) {
            case rtc::Description::Direction::SendOnly: dir = "sendonly"; break;
            case rtc::Description::Direction::RecvOnly: dir = "recvonly"; break;
            case rtc::Description::Direction::SendRecv: dir = "sendrecv"; break;
            default: dir = "inactive"; break;
          }
          fprintf(stderr, "[host] attach sender (track open=%d dir=%s)\n",
                  (int)track->isOpen(), dir);
          fflush(stderr);
          rdDebug("session", "host attach sender open=%d dir=%s",
                  (int)track->isOpen(), dir);
          sendTrack_ = track;
          sender_.attach(track, [this] {
            keyframeRequested_ = true;  // receiver asked for an IDR (PLI/FIR)
          });
          keyframeRequested_ = true;  // decoder needs an IDR to start
        },
        Qt::QueuedConnection);
  });

  connect(peer_.get(), &PeerSession::connected, this, [this] {
    rdDebug("session", "connected (role=host)");
    keyframeRequested_ = true;  // (re)connect: force an IDR
    emit connected();
  }, Qt::QueuedConnection);
  connect(peer_.get(), &PeerSession::disconnected, this,
          &RemoteDesktopSession::disconnected, Qt::QueuedConnection);
  connect(peer_.get(), &PeerSession::failed, this,
          &RemoteDesktopSession::failed, Qt::QueuedConnection);

  encoder_ = std::make_unique<H264Encoder>();
  const bool encOk = encoder_->init(kInitialEncodeWidth, kInitialEncodeHeight,
                                    kCaptureFps, kInitialBitrateKbps);
  if (!encOk) {
    stop();
    emit failed(QStringLiteral("编码器初始化失败"));
    return false;
  }
  encodeW_.store(kInitialEncodeWidth, std::memory_order_relaxed);
  encodeH_.store(kInitialEncodeHeight, std::memory_order_relaxed);
  keyframeRequested_ = true;
  hostRunning_ = true;
  hostThread_ = std::make_unique<std::thread>([this] { hostLoop(); });
  return true;
}

QString RemoteDesktopSession::hostAcceptOffer(const QString& offerSdp) {
  if (role_ != Role::Host || !peer_) {
    return {};
  }
  return peer_->acceptOfferAndCreateAnswer(offerSdp);
}

QString RemoteDesktopSession::hostAcceptOfferChecked(const QString& offerSdp,
                                                     int64_t screenId) {
  if (!startHost(screenId)) {
    return {};
  }
  const QString answer = hostAcceptOffer(offerSdp);
  if (answer.isEmpty()) {
    stop();
    return {};
  }
  return answer;
}

bool RemoteDesktopSession::startViewer() {
  if (role_ != Role::None) {
    return false;
  }
  role_ = Role::Viewer;
  resetDiag();
  peer_ = std::make_unique<PeerSession>();
  peer_->start(/*isOfferer=*/true, /*sendVideo=*/false);
  control_->createOn(peer_->pc());

  connect(peer_.get(), &PeerSession::connected, this,
          [this] {
            rdDebug("session", "connected (role=viewer)");
            // Announce the viewer's quality choice once the channel is up.
            if (control_ && control_->isOpen()) {
              control_->sendQualityMode(qualityMode_.load());
            }
            emit connected();
          },
          Qt::QueuedConnection);
  connect(peer_.get(), &PeerSession::disconnected, this,
          &RemoteDesktopSession::disconnected, Qt::QueuedConnection);
  connect(peer_.get(), &PeerSession::failed, this,
          &RemoteDesktopSession::failed, Qt::QueuedConnection);

  decoder_ = std::make_unique<H264Decoder>();
  if (!decoder_->init()) {
    rdDebug("session", "viewer decoder init FAILED");
    emit failed(QStringLiteral("解码器初始化失败"));
    return false;
  }

  // Per-second video stats upload feeding the host's quality ladder.
  delete statsTimer_;
  statsTimer_ = new QTimer(this);
  statsTimer_->setInterval(1000);
  connect(statsTimer_, &QTimer::timeout, this,
          &RemoteDesktopSession::sendVideoStatsTick);
  statsTimer_->start();
  // The control channel may open after PC-connected; announce the quality
  // choice then as well (connected-time announce can race the DC open).
  connect(control_, &ControlChannel::opened, this, [this] {
    if (role_ == Role::Viewer) {
      control_->sendQualityMode(qualityMode_.load(std::memory_order_relaxed));
    }
  });

  // The pre-added RecvOnly track (localTrack) IS the receiving track: for a
  // mid that already has a local track, libdatachannel does not fire onTrack.
  std::shared_ptr<rtc::Track> track = peer_->localTrack();
  if (!track) {
    emit failed(QStringLiteral("视频轨道创建失败"));
    return false;
  }
  const auto alive = alive_;
  track->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>());
  track->onOpen([] {
    fprintf(stderr, "[viewer] track open\n");
    fflush(stderr);
  });
  // Depacketized video frames carry a FrameInfo, so they are delivered to
  // onFrame; onMessage would silently drop them (libdatachannel >= 0.20).
  track->onOpen([] {
    fprintf(stderr, "[viewer] track open\n");
    fflush(stderr);
    rdDebug("session", "viewer track open");
  });
  // Depacketized video frames carry a FrameInfo, so they are delivered to
  // onFrame; onMessage would silently drop them (libdatachannel >= 0.20).
  track->onFrame(
      [this, alive](rtc::binary data, [[maybe_unused]] rtc::FrameInfo info) {
        received_.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> dbgCount{0};
        if (dbgCount.fetch_add(1) < 3) {
          fprintf(stderr, "[viewer] frame #%d size=%zu\n", dbgCount.load(),
                  data.size());
          fflush(stderr);
          rdDebug("session", "viewer received frame #%d size=%zu",
                  dbgCount.load(), data.size());
        }
        if (!alive->load(std::memory_order_acquire)) {
          return;
        }
        if (data.empty()) {
          return;
        }
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data.data());
        const size_t size = data.size();
        std::vector<uint8_t> yBuf, uBuf, vBuf;
        int yStride = 0, uvStride = 0;
        I420Frame frame;
        bool decoded = false;
        {
          // stop() may be closing the decoder on the UI thread while a
          // message is in flight on this thread.
          std::lock_guard<std::mutex> lock(decoderMtx_);
          if (!decoder_) {
            return;
          }
          decoded = decoder_->decode(ptr, size, frame, yBuf, uBuf, vBuf,
                                     yStride, uvStride);
        }
        if (!decoded) {
          decodeFailed_.fetch_add(1, std::memory_order_relaxed);
          static std::atomic<int> dbgFails{0};
          if (dbgFails.fetch_add(1) < 3) {
            fprintf(stderr, "[viewer] decode failed #%d (size=%zu)\n",
                    dbgFails.load(), size);
            fflush(stderr);
            rdDebug("session", "viewer decode failed #%d (size=%zu)",
                    dbgFails.load(), size);
          }
          return;
        }
        decoded_.fetch_add(1, std::memory_order_relaxed);
        // Every 120 frames (~5s): pipeline heartbeat into rd_debug.log so a
        // stalled stage is visible without UI counters.
        if (received_.load(std::memory_order_relaxed) % 120 == 1) {
          rdDebug("session", "viewer diag received=%u decoded=%u failed=%u",
                  received_.load(), decoded_.load(), decodeFailed_.load());
        }
        QImage img(frame.width, frame.height, QImage::Format_RGB32);
        const int rgbStride = img.bytesPerLine();
        I420ToBgra(yBuf.data(), yStride, uBuf.data(), uvStride, vBuf.data(),
                   uvStride, frame.width, frame.height, img.bits(), rgbStride);
        // RD_DUMP_FIRST_FRAME=<path>: save the first decoded frame as PNG
        // (diagnostics: proves whether capture itself produces black pixels).
        static std::atomic<bool> dumped{false};
        if (!dumped.exchange(true)) {
          const QString dumpPath =
              qEnvironmentVariable("RD_DUMP_FIRST_FRAME");
          if (!dumpPath.isEmpty()) {
            img.copy().save(dumpPath, "PNG");
            rdDebug("session", "dumped first decoded frame %dx%d -> %s",
                    img.width(), img.height(), dumpPath.toUtf8().constData());
          }
        }
        emit frameReady(img.copy());
      });
  return true;
}

QString RemoteDesktopSession::viewerCreateOffer() {
  if (role_ != Role::Viewer || !peer_) {
    return {};
  }
  return peer_->createOffer();
}

bool RemoteDesktopSession::viewerAcceptAnswer(const QString& answerSdp) {
  if (role_ != Role::Viewer || !peer_) {
    return false;
  }
  return peer_->acceptAnswer(answerSdp);
}

void RemoteDesktopSession::stop() {
  // Order matters: (1) flip the alive flag so libdatachannel-thread callbacks
  // bail out, (2) close the peer connection so its callback threads stop
  // touching decoder/sender/track, (3) tear down threads and codec objects.
  // Tearing down first was a use-after-free (callbacks kept firing).
  alive_->store(false, std::memory_order_release);
  if (peer_) {
    peer_->close();
    peer_.reset();
  }
  stopThreads();
  role_ = Role::None;
}

void RemoteDesktopSession::stopThreads() {
  hostRunning_ = false;
  // The host thread parks on this cv while waiting for frames; the predicate
  // also covers hostRunning_, so a missed wake-up is impossible, but the
  // notify ends the wait immediately instead of after the next frame.
  encQueueCv_.notify_all();
  if (hostThread_ && hostThread_->joinable()) {
    hostThread_->join();
  }
  hostThread_.reset();
  capture_.reset();
  {
    std::lock_guard<std::mutex> lock(encQueueMtx_);
    encQueue_.clear();
  }
  sender_.detach();
  sendTrack_.reset();
  if (statsTimer_) {
    statsTimer_->stop();
    statsTimer_->deleteLater();
    statsTimer_ = nullptr;
  }
  if (encoder_) {
    encoder_->close();
    encoder_.reset();
  }
  std::lock_guard<std::mutex> lock(decoderMtx_);
  if (decoder_) {
    decoder_->close();
    decoder_.reset();
  }
}

void RemoteDesktopSession::hostLoop() {
  fprintf(stderr, "[loop] enter\n"); fflush(stderr);

  // Encoder-thread state (this loop is the only consumer of the queue).
  int tickFrames = 0;
  int encodedThisTick = 0;
  int fpsAccumulator = 0;
  int configuredFps = 0;

  // Sink that lives as long as the loop does; the capture thread joins
  // before this stack frame ends (stop() -> stopThreads() -> join). It only
  // copies the raw BGRA into the encode queue: conversion and encode time
  // can never stall the capture cadence.
  class HostLoopSink : public ::rd::FrameSink {
   public:
    explicit HostLoopSink(RemoteDesktopSession* s) : s_(s) {}
    void onFrame(const scl_frame& raw) override {
      s_->captured_.fetch_add(1, std::memory_order_relaxed);
      PendingFrame pf;
      pf.width = raw.width;
      pf.height = raw.height;
      pf.stride = raw.stride;
      pf.originX = static_cast<int>(raw.top_left_x);
      pf.originY = static_cast<int>(raw.top_left_y);
      pf.bgra.resize(static_cast<size_t>(raw.stride) *
                     static_cast<size_t>(raw.height));
      memcpy(pf.bgra.data(), raw.data, pf.bgra.size());
      bool dropped = false;
      {
        std::lock_guard<std::mutex> lock(s_->encQueueMtx_);
        if (s_->encQueue_.size() >= kEncQueueCapacity) {
          // Encoder is behind: drop the STALEST queued frame and keep the
          // freshest screen content (latency never accumulates). Arbitrary
          // dropped frames are safe between P frames — the encoder just
          // encodes fewer of them.
          s_->encQueue_.pop_front();
          dropped = true;
        }
        s_->encQueue_.push_back(std::move(pf));
      }
      s_->encQueueCv_.notify_one();
      if (dropped) {
        const uint32_t total =
            s_->dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (total % kCaptureFps == 1) {  // at most ~1 line per second
          rdDebug("encode", "encode queue full: dropped total=%u", total);
        }
      }
    }
    void onGeometry(int ox, int oy, int w, int h) override {
      static std::atomic<int> dbg{0};
      if (dbg.fetch_add(1) < 3) {
        rdDebug("session", "geometry origin=(%d,%d) size=%dx%d", ox, oy, w, h);
      }
      // Geometry is applied on the encoder thread from the queued frame
      // payload, so the quality ladder state stays single-threaded.
    }

   private:
    RemoteDesktopSession* s_;
  };

  capture_ = std::make_unique<ScreenCaptureThread>();
  fprintf(stderr, "[loop] capture created\n");
  fflush(stderr);
  HostLoopSink sink(this);
  fprintf(stderr, "[loop] starting capture\n");
  fflush(stderr);
  if (!capture_->start(screenId_, kCaptureFps, &sink)) {
    rdDebug("session", "capture start FAILED (screen=%lld)",
            (long long)screenId_);
    QMetaObject::invokeMethod(this, "failed", Qt::QueuedConnection,
                              Q_ARG(QString, QStringLiteral("屏幕捕获启动失败")));
    return;
  }
  rdDebug("session", "capture started (screen=%lld, %dfps)",
          (long long)screenId_, kCaptureFps);
  fprintf(stderr, "[loop] capture started, encoding\n");
  fflush(stderr);

  // Encoder consumer loop. P frames chain strictly through this one thread,
  // so the bitstream is identical to a serial pipeline; what overlaps is
  // whole frames (capture of frame N+1 while frame N is encoded/sent).
  while (true) {
    PendingFrame pf;
    {
      std::unique_lock<std::mutex> lock(encQueueMtx_);
      encQueueCv_.wait(lock,
                       [&] { return !hostRunning_.load() || !encQueue_.empty(); });
      if (!hostRunning_.load()) {
        break;
      }
      pf = std::move(encQueue_.front());
      encQueue_.pop_front();
    }

    if (!encoder_) {
      continue;  // RD_TEST_NO_ENC: exercise capture/queue handoff only
    }

    const auto t0 = std::chrono::steady_clock::now();
    quality_.setNativeSize(pf.width, pf.height);
    injector_.setFrameOrigin(pf.originX, pf.originY);
    injector_.setFrameSize(pf.width, pf.height);

    // Once per second: drain viewer stats, run the quality ladder, and
    // reconfigure the encoder if the level changed. The tick counts frames
    // handed to the encoder; queue drops only occur when the encoder is the
    // bottleneck, so they merely stretch the tick cadence under load.
    if (++tickFrames >= kCaptureFps) {
      tickFrames = 0;
      const int recv = statRecv_.exchange(0, std::memory_order_relaxed);
      const int dec = statDec_.exchange(0, std::memory_order_relaxed);
      const int fail = statFail_.exchange(0, std::memory_order_relaxed);
      if (recv | dec | fail) {
        quality_.onViewerStats(recv, dec, fail);
      }
      const int desired = quality_.evaluate(qualityMode_.load(),
                                            encodedThisTick);
      encodedThisTick = 0;
      int w = 0, h = 0;
      quality_.sizeFor(desired, &w, &h);
      const int kbps = VideoQualityController::bitrateKbpsFor(desired);
      const int fps = VideoQualityController::fpsFor(desired);
      if (w != encoder_->width() || h != encoder_->height() ||
          kbps != encoder_->bitrateKbps() || fps != configuredFps) {
        if (encoder_->reconfigure(w, h, fps, kbps)) {
          configuredFps = fps;
          keyframeRequested_ = true;  // decoder needs the new SPS/PPS
          encodeW_.store(w, std::memory_order_relaxed);
          encodeH_.store(h, std::memory_order_relaxed);
          if (control_ && control_->isOpen()) {
            control_->sendFrameMeta(w, h);  // viewer diagnostics
          }
          rdDebug("quality", "encoder reconfigured to %dx%d @%dkbps %dfps",
                  w, h, kbps, fps);
        }
      }
    }

    // Frame gating: encode at the level's fps (accumulator keeps the dropped
    // frames evenly spread; capture stays at kCaptureFps).
    fpsAccumulator += VideoQualityController::fpsFor(quality_.level());
    if (fpsAccumulator < kCaptureFps) {
      continue;
    }
    fpsAccumulator -= kCaptureFps;

    bool forceKey = keyframeRequested_.exchange(false);
    if (sender_.consumeOpenTransition()) {
      // The track just became open: the viewer may have joined mid-GOP, so
      // start the stream with an IDR.
      forceKey = true;
    }
    // Convert at the encoder's current size (the ladder may reconfigure it
    // between frames; encode() also guards on a size mismatch).
    scl_frame view{};
    view.width = pf.width;
    view.height = pf.height;
    view.stride = pf.stride;
    view.data = pf.bgra.data();
    view.top_left_x = pf.originX;
    view.top_left_y = pf.originY;
    I420Frame i420 = ConvertBgraToI420(view, encoder_->width(),
                                       encoder_->height());
    std::vector<uint8_t> au = encoder_->encode(i420, forceKey);
    const auto t1 = std::chrono::steady_clock::now();
    quality_.onEncodedFrame(
        std::chrono::duration<double, std::milli>(t1 - t0).count());
    encodeEmaMs_.store(static_cast<float>(quality_.emaMs()));
    if (!au.empty()) {
      ++encodedThisTick;
      encoded_.fetch_add(1, std::memory_order_relaxed);
      sender_.send(au, forceKey);
      sent_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  // Stop the capture thread BEFORE the sink dies: leaving it to stopThreads()
  // would let the capture thread call onFrame() on this destroyed stack
  // object (the classic teardown crash).
  capture_->stop();
  rdDebug("session", "encode loop exit");
  fprintf(stderr, "[loop] exit\n");
  fflush(stderr);
}

void RemoteDesktopSession::onInputReceived(InputEvent ev) {
  static std::atomic<int> dbg{0};
  if (dbg.fetch_add(1) < 10) {
    rdDebug("session", "inputReceived role=%s type=%d", role_ == Role::Host ? "host" : "other",
            (int)ev.type);
  }
  if (role_ != Role::Host) {
    return;
  }
  injector_.inject(ev);
  emit inputInjected();
}

}  // namespace rd
