// remote_desktop — headless end-to-end self test.
//
// Runs one process with two sessions (host + viewer), exchanges SDP through
// memory, and verifies: connection established, video frames decoded on the
// viewer, and an input event injected on the host. Exit code 0 = all passed.
#include <cstdio>

#include <QCoreApplication>
#include <QTimer>

#include <rtc/rtc.hpp>

#include "session/remote_desktop_session.h"

using namespace rd;

// Test-only access to drive the host capture pipeline without a peer.
// RD_TEST_NO_PEER_SESSION / RD_TEST_NO_ENC now handled inside the
// engine (EngineImpl ctor, engine/src/engine.cc); just run startHost.
namespace rd {
struct SelfTestHooks {
  static void runHostPipelineOnly(RemoteDesktopSession& s) { s.startHost(0); }
  // 模拟采集线程的锁屏状态变化（真锁屏无法在自测中触发）。
  static void pushSecureDesktopState(RemoteDesktopSession& s, bool secure,
                                     bool capturable) {
    s.HandleSecureDesktopChange(secure, capturable);
  }
};
}  // namespace rd

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("RemoteDesktopSelfTest"));

  if (!qEnvironmentVariableIsEmpty("RD_TEST_RTC_LOG")) {
    rtc::InitLogger(rtc::LogLevel::Debug);
  }

  RemoteDesktopSession host;
  RemoteDesktopSession viewer;

  int framesReceived = 0;
  int inputsInjected = 0;
  int peerSecureEvents = 0;
  bool peerSecureOk = false;
  bool connected = false;
  QStringList failures;

  QObject::connect(&viewer, &RemoteDesktopSession::frameReady,
                   [&framesReceived](const QImage&) { ++framesReceived; });
  QObject::connect(&host, &RemoteDesktopSession::inputInjected,
                   [&inputsInjected]() { ++inputsInjected; });
  // &app 上下文：connected 由引擎派发线程发出，无上下文的 lambda 会在该
  // 线程直连执行，其中的 QTimer::singleShot 不会触发（非 Qt 线程）。
  QObject::connect(&viewer, &RemoteDesktopSession::connected, &app, [&]() {
    connected = true;
    // The control DataChannel opens with the connection; inject a few input
    // events from the viewer so the host-side injection path is exercised.
    // （通道未开时引擎静默丢弃——与旧 control()->sendInput 行为一致。）
    InputEvent ev;
    ev.type = InputEvent::Type::MouseMove;
    ev.nx = 0.5;
    ev.ny = 0.5;
    viewer.sendInput(ev);
    // 锁屏状态端到端：host 采集线程事件 -> 控制通道 -> viewer 信号。
    QTimer::singleShot(500, &app, [&]() {
      SelfTestHooks::pushSecureDesktopState(host, true, false);
    });
    QTimer::singleShot(1000, &app, [&]() {
      SelfTestHooks::pushSecureDesktopState(host, false, true);
    });
    QTimer::singleShot(300, &app, [&]() {
      InputEvent click;
      click.type = InputEvent::Type::MouseButton;
      click.nx = 0.5;
      click.ny = 0.5;
      click.button = 1;
      click.pressed = true;
      viewer.sendInput(click);
    });
  });
  QObject::connect(&viewer, &RemoteDesktopSession::peerSecureDesktopChanged,
                   [&](bool secure, bool capturable) {
                     ++peerSecureEvents;
                     if (peerSecureEvents == 1) {
                       peerSecureOk = secure && !capturable;
                     } else if (peerSecureEvents == 2) {
                       peerSecureOk = peerSecureOk && !secure && capturable;
                     }
                   });
  QObject::connect(&host, &RemoteDesktopSession::failed,
                   [&failures](const QString& r) { failures << "host: " + r; });
  QObject::connect(&viewer, &RemoteDesktopSession::failed,
                   [&failures](const QString& r) { failures << "viewer: " + r; });

  // RemoteDesktopSession emits queued signals; move both to the app thread.
  host.moveToThread(app.thread());
  viewer.moveToThread(app.thread());

  QTimer::singleShot(0, &app, [&]() {
    // 1. Viewer offers, host answers, viewer accepts —all through memory.
    if (qEnvironmentVariableIsEmpty("RD_TEST_NO_VIEWER")) {
    fprintf(stderr, "[phase] startViewer...\n"); fflush(stderr);
    if (!viewer.startViewer()) {
      failures << "viewer init failed";
    }
    }
    fprintf(stderr, "[phase] startHost...\n"); fflush(stderr);
    if (!host.startHost(/*screenId=*/0)) {
      failures << "host init failed";
    }
    if (failures.isEmpty() && !qEnvironmentVariableIsEmpty("RD_TEST_NO_SDP")) {
      qInfo("RD_TEST_NO_SDP set: skipping SDP exchange");
      return;
    }
    if (failures.isEmpty()) {
      fprintf(stderr, "[phase] createOffer...\n"); fflush(stderr);
      const QString offer = viewer.viewerCreateOffer();
      if (offer.isEmpty()) {
        failures << "empty offer";
      } else {
        fprintf(stderr, "[phase] hostAcceptOffer...\n"); fflush(stderr);
        const QString answer = host.hostAcceptOffer(offer);
        if (answer.isEmpty()) {
          failures << "empty answer";
        } else if (!viewer.viewerAcceptAnswer(answer)) {
          failures << "answer rejected";
        }
      }
    }
  });

  // 2. Judge after 12 seconds of runtime.
  QTimer::singleShot(12000, &app, [&]() {
    fprintf(stderr, "=== remote_desktop self test ===\n");
    fprintf(stderr, "connected     : %s\n", connected ? "YES" : "NO");
    fprintf(stderr, "frames decoded: %d\n", framesReceived);
    fprintf(stderr, "inputs inject : %d\n", inputsInjected);
    fprintf(stderr, "secure events : %d (ok=%s)\n", peerSecureEvents,
            peerSecureOk ? "YES" : "NO");
    for (const QString& f : failures) {
      fprintf(stderr, "failure       : %s\n", qUtf8Printable(f));
    }
    bool pass = connected && framesReceived > 0 && inputsInjected > 0 &&
                peerSecureEvents == 2 && peerSecureOk && failures.isEmpty();
    // Input injection requires the control channel; report separately so a
    // video-only pass is still diagnosable.
    fprintf(stderr, "RESULT        : %s\n", pass ? "PASS" : "FAIL");
    fflush(stderr); fflush(stdout);
    ::_exit(pass ? 0 : 1);  // skip static/stack teardown (join crash is teardown-only)
  });

  return app.exec();
}
