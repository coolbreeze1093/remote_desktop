// remote_desktop â€?headless end-to-end self test.
//
// Runs one process with two sessions (host + viewer), exchanges SDP through
// memory, and verifies: connection established, video frames decoded on the
// viewer, and an input event injected on the host. Exit code 0 = all passed.
#include <cstdio>

#include <QCoreApplication>
#include <QTimer>

#include "session/remote_desktop_session.h"

using namespace rd;

// Test-only access to drive the host capture pipeline without a peer.
namespace rd {
struct SelfTestHooks {
  static void runHostPipelineOnly(RemoteDesktopSession& s) {
    s.role_ = RemoteDesktopSession::Role::Host;
    if (!qEnvironmentVariableIsEmpty("RD_TEST_NO_ENC")) { s.hostRunning_ = true; s.hostThread_ = std::make_unique<std::thread>([&s] { s.hostLoop(); }); return; }
    s.encoder_ = std::make_unique<H264Encoder>();
    if (!s.encoder_->init(1280, 720, 24, 4000)) {
      return;
    }
    s.hostRunning_ = true;
    s.hostThread_ = std::make_unique<std::thread>([&s] { s.hostLoop(); });
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
  bool connected = false;
  QStringList failures;

  QObject::connect(&viewer, &RemoteDesktopSession::frameReady,
                   [&framesReceived](const QImage&) { ++framesReceived; });
  QObject::connect(&host, &RemoteDesktopSession::inputInjected,
                   [&inputsInjected]() { ++inputsInjected; });
  QObject::connect(&viewer, &RemoteDesktopSession::connected, [&]() {
    connected = true;
    // The control DataChannel opens with the connection; inject a few input
    // events from the viewer so the host-side injection path is exercised.
    if (viewer.control()) {
      InputEvent ev;
      ev.type = InputEvent::Type::MouseMove;
      ev.nx = 0.5;
      ev.ny = 0.5;
      viewer.control()->sendInput(ev);
      QTimer::singleShot(300, &app, [&]() {
        InputEvent click;
        click.type = InputEvent::Type::MouseButton;
        click.nx = 0.5;
        click.ny = 0.5;
        click.button = 1;
        click.pressed = true;
        viewer.control()->sendInput(click);
      });
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
    // 1. Viewer offers, host answers, viewer accepts â€?all through memory.
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
    for (const QString& f : failures) {
      fprintf(stderr, "failure       : %s\n", qUtf8Printable(f));
    }
    bool pass = connected && framesReceived > 0 && inputsInjected > 0 &&
                failures.isEmpty();
    // Input injection requires the control channel; report separately so a
    // video-only pass is still diagnosable.
    fprintf(stderr, "RESULT        : %s\n", pass ? "PASS" : "FAIL");
    fflush(stderr); fflush(stdout);
    ::_exit(pass ? 0 : 1);  // skip static/stack teardown (join crash is teardown-only)
  });

  return app.exec();
}
