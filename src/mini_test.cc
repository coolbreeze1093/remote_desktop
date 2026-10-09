// remote_desktop — GUI repro harness for the "minimize freezes the remote"
// report. One process runs a full host+viewer pipeline (in-memory SDP
// exchange). Real windows on both sides (owner+RemoteVideoWindow on the
// viewer, a plain main window on the host), then a script drives every
// minimize variant: video window from normal, from fullscreen, its owner,
// and the HOST app window — logging once a second whether (a) the stream
// still delivers frames and (b) the video window still paints.
#include <cstdio>

#include <QApplication>
#include <QEvent>
#include <QMainWindow>
#include <QTimer>

#include "session/remote_desktop_session.h"
#include "ui/remote_video_window.h"

using namespace rd;

namespace {

// Counts paint events per top-level widget (installed on the app object).
class PaintCounter : public QObject {
 public:
  int videoPaints = 0;
  int hostPaints = 0;
  QWidget* videoTop = nullptr;
  QWidget* hostTop = nullptr;

  bool eventFilter(QObject* watched, QEvent* ev) override {
    if (ev->type() == QEvent::Paint) {
      auto* w = qobject_cast<QWidget*>(watched);
      if (w && videoTop && w->window() == videoTop) {
        ++videoPaints;
      } else if (w && hostTop && w->window() == hostTop) {
        ++hostPaints;
      }
    } else if (ev->type() == QEvent::WindowStateChange) {
      auto* w = qobject_cast<QWidget*>(watched);
      if (w && ((videoTop && w->window() == videoTop) ||
                (hostTop && w->window() == hostTop))) {
        const char* who = videoTop && w->window() == videoTop ? "video" : "host";
        QString st;
        if (w->windowState() & Qt::WindowMinimized) st = "min";
        else if (w->windowState() & Qt::WindowFullScreen) st = "fs";
        else st = "normal";
        fprintf(stderr, "[evt] %s windowState -> %s\n", who,
                st.toLocal8Bit().constData());
        fflush(stderr);
      }
    }
    return QObject::eventFilter(watched, ev);
  }
};

int lastVideoPaints = 0;
int lastHostPaints = 0;
uint32_t lastReceived = 0;
uint32_t lastCaptured = 0;

void logStatus(RemoteDesktopSession* host, RemoteDesktopSession* viewer,
               PaintCounter* counter) {
  const auto h = host->diagStats();
  const auto v = viewer->diagStats();
  fprintf(stderr,
          "[status] host cap/enc/sent=%u/%u/%u (+cap %u) | viewer recv=%u"
          " (+%u) dec=%u fail=%u | paints video=%d(+%d) host=%d(+%d)\n",
          h.captured, h.encoded, h.sent, h.captured - lastCaptured, v.received,
          v.received - lastReceived, v.decoded, v.decodeFailed,
          counter->videoPaints, counter->videoPaints - lastVideoPaints,
          counter->hostPaints, counter->hostPaints - lastHostPaints);
  fflush(stderr);
  lastVideoPaints = counter->videoPaints;
  lastHostPaints = counter->hostPaints;
  lastReceived = v.received;
  lastCaptured = h.captured;
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("RemoteDesktopMiniTest"));

  // Optional libdatachannel trace for catching the flaky "viewer receives
  // nothing" run; goes to stderr alongside the status lines. Verbose logs
  // every SRTP protect/unprotect so the packet flow is traceable.
  if (!qEnvironmentVariableIsEmpty("RD_TEST_RTC_LOG")) {
    rtc::InitLogger(rtc::LogLevel::Verbose);
  }

  RemoteDesktopSession host;
  RemoteDesktopSession viewer;

  QMainWindow owner;      // viewer side: stands in for MainWindow
  RemoteVideoWindow videoWin(&owner);
  owner.resize(400, 300);
  owner.show();

  QMainWindow hostWin;    // host side: the app window the peer could minimize
  hostWin.setWindowTitle(QStringLiteral("host app window"));
  hostWin.resize(400, 300);
  hostWin.show();

  PaintCounter counter;
  counter.videoTop = &videoWin;
  counter.hostTop = &hostWin;
  app.installEventFilter(&counter);

  QObject::connect(&viewer, &RemoteDesktopSession::frameReady, &videoWin,
                   &RemoteVideoWindow::setFrame, Qt::QueuedConnection);

  QTimer statusTimer;
  QObject::connect(&statusTimer, &QTimer::timeout, &app,
                   [&] { logStatus(&host, &viewer, &counter); });
  statusTimer.start(1000);

  QTimer::singleShot(0, &app, [&] {
    fprintf(stderr, "[phase] starting sessions\n");
    fflush(stderr);
    if (!viewer.startViewer()) {
      fprintf(stderr, "[FATAL] viewer init failed\n");
      ::_exit(1);
    }
    if (!host.startHost(/*screenId=*/0)) {
      fprintf(stderr, "[FATAL] host init failed\n");
      ::_exit(1);
    }
    const QString offer = viewer.viewerCreateOffer();
    const QString answer = host.hostAcceptOffer(offer);
    if (answer.isEmpty() || !viewer.viewerAcceptAnswer(answer)) {
      fprintf(stderr, "[FATAL] SDP exchange failed\n");
      ::_exit(1);
    }
    fprintf(stderr, "[phase] SDP exchanged, waiting for connect\n");
    fflush(stderr);
  });

  // Scripted window choreography, mirroring the user's flow. RD_MINI_QUICK=1
  // skips it and only judges whether the stream comes up at all (flakiness
  // measurement of the "sender sends, receiver never fires" failure).
  const bool quick = !qEnvironmentVariableIsEmpty("RD_MINI_QUICK");
  if (!quick) {
  QTimer::singleShot(4000, &app, [&] {
    fprintf(stderr, "=== STEP 1: video -> showFullscreen\n"); fflush(stderr);
    videoWin.showFullscreen();
  });
  QTimer::singleShot(8000, &app, [&] {
    fprintf(stderr, "=== STEP 2: video -> showNormal\n"); fflush(stderr);
    videoWin.showNormal();
  });
  QTimer::singleShot(10000, &app, [&] {
    fprintf(stderr, "=== STEP 3: video -> showMinimized (from normal)\n");
    fflush(stderr);
    videoWin.showMinimized();
  });
  QTimer::singleShot(13000, &app, [&] {
    fprintf(stderr, "=== STEP 4: video -> showNormal (restore)\n");
    fflush(stderr);
    videoWin.showNormal();
  });
  QTimer::singleShot(15000, &app, [&] {
    fprintf(stderr, "=== STEP 5: video -> showFullScreen again\n");
    fflush(stderr);
    videoWin.showFullScreen();
  });
  QTimer::singleShot(17000, &app, [&] {
    fprintf(stderr, "=== STEP 6: video -> showMinimized (FROM FULLSCREEN)\n");
    fflush(stderr);
    videoWin.showMinimized();
  });
  QTimer::singleShot(20000, &app, [&] {
    fprintf(stderr, "=== STEP 7: video -> showNormal (restore from fs-min)\n");
    fflush(stderr);
    videoWin.showNormal();
  });
  QTimer::singleShot(22000, &app, [&] {
    fprintf(stderr, "=== STEP 8: owner (viewer main) -> showMinimized\n");
    fflush(stderr);
    owner.showMinimized();
  });
  QTimer::singleShot(25000, &app, [&] {
    fprintf(stderr, "=== STEP 9: owner -> showNormal\n"); fflush(stderr);
    owner.showNormal();
    videoWin.raise();
  });
  QTimer::singleShot(27000, &app, [&] {
    fprintf(stderr, "=== STEP 10: HOST app window -> showMinimized\n");
    fflush(stderr);
    hostWin.showMinimized();
  });
  QTimer::singleShot(31000, &app, [&] {
    fprintf(stderr, "=== STEP 11: HOST app window -> showNormal\n");
    fflush(stderr);
    hostWin.showNormal();
  });
  }

  QTimer::singleShot(quick ? 12000 : 34000, &app, [&] {
    const auto v = viewer.diagStats();
    const auto h = host.diagStats();
    fprintf(stderr,
            "=== judge: recvDelta last 1s=%u captureDelta=%u paints video=%d "
            "host=%d | totals recv=%u dec=%u fail=%u cap=%u enc=%u sent=%u\n",
            v.received - lastReceived, h.captured - lastCaptured,
            counter.videoPaints - lastVideoPaints,
            counter.hostPaints - lastHostPaints, v.received, v.decoded,
            v.decodeFailed, h.captured, h.encoded, h.sent);
    fflush(stderr);
    ::_exit(v.received > 0 ? 0 : 2);  // 2 = stream never delivered a frame
  });

  return app.exec();
}
