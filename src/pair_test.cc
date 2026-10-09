// remote_desktop — headless two-process pair test.
//
// Viewer process: rd_pair_test.exe viewer   → writes pair_offer.txt, waits for
//                                            pair_answer.txt, applies it.
// Host process:   rd_pair_test.exe host     → waits for pair_offer.txt, starts
//                                            host session with it, writes
//                                            pair_answer.txt.
// Both print counters and exit 0 when the viewer decoded frames. SDP files
// live in the CWD.
#include <cstdio>
#include <QCoreApplication>
#include <QFile>
#include <QThread>
#include <QTimer>

#include "session/remote_desktop_session.h"

using namespace rd;

static bool waitForFile(const QString& path, QString* out, int timeoutMs) {
  for (int waited = 0; waited < timeoutMs; waited += 200) {
    QFile f(path);
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
      const QString s = QString::fromUtf8(f.readAll()).trimmed();
      if (!s.isEmpty()) {
        *out = s;
        return true;
      }
    }
    QThread::msleep(200);
  }
  return false;
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("RemoteDesktopPairTest"));
  const QString role = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
  if (role != QLatin1String("viewer") && role != QLatin1String("host")) {
    fprintf(stderr, "usage: rd_pair_test viewer|host\n");
    return 2;
  }

  RemoteDesktopSession session;
  session.moveToThread(app.thread());

  int frames = 0;
  bool connected = false;
  QStringList failures;
  QObject::connect(&session, &RemoteDesktopSession::frameReady,
                   [&frames](const QImage&) { ++frames; });
  QObject::connect(&session, &RemoteDesktopSession::connected,
                   [&connected]() {
                     connected = true;
                     fprintf(stderr, "[pair] connected\n");
                     fflush(stderr);
                   });
  QObject::connect(&session, &RemoteDesktopSession::failed,
                   [&failures](const QString& r) {
                     failures << r;
                     fprintf(stderr, "[pair] failed: %s\n", qUtf8Printable(r));
                     fflush(stderr);
                   });

  QTimer::singleShot(0, &app, [&]() {
    if (role == QLatin1String("viewer")) {
      if (!session.startViewer()) {
        failures << "viewer init failed";
        return;
      }
      // Optional manual quality pick (tests the viewer→host quality path).
      bool ok = false;
      const int q = qEnvironmentVariableIntValue("RD_TEST_QUALITY", &ok);
      if (ok && q >= 0 && q <= 3) {
        session.setVideoQualityMode(q);
      }
      const QString offer = session.viewerCreateOffer();
      if (offer.isEmpty()) {
        failures << "empty offer";
        return;
      }
      QFile::remove("pair_offer.txt");
      QFile f("pair_offer.txt");
      f.open(QIODevice::WriteOnly);
      f.write(offer.toUtf8());
      f.close();
      fprintf(stderr, "[pair] offer written (%d chars), waiting answer...\n",
              offer.size());
      fflush(stderr);
      QString answer;
      if (!waitForFile("pair_answer.txt", &answer, 60000)) {
        failures << "no answer file";
        return;
      }
      if (!session.viewerAcceptAnswer(answer)) {
        failures << "answer rejected";
      }
    } else {
      fprintf(stderr, "[pair] host waiting for offer...\n");
      fflush(stderr);
      QString offer;
      if (!waitForFile("pair_offer.txt", &offer, 60000)) {
        failures << "no offer file";
        return;
      }
      const QString answer = session.hostAcceptOfferChecked(offer, 0);
      if (answer.isEmpty()) {
        failures << "empty answer (startHost or SDP failed)";
        return;
      }
      QFile f("pair_answer.txt");
      f.open(QIODevice::WriteOnly);
      f.write(answer.toUtf8());
      f.close();
      fprintf(stderr, "[pair] answer written (%d chars)\n", answer.size());
      fflush(stderr);
    }
  });

  const int judgeMs = 20000;
  QTimer::singleShot(judgeMs, &app, [&]() {
    const auto st = session.diagStats();
    fprintf(stderr, "=== pair test (%s) ===\n", qUtf8Printable(role));
    fprintf(stderr, "connected : %s\n", connected ? "YES" : "NO");
    if (role == QLatin1String("viewer")) {
      fprintf(stderr, "received  : %u\n", st.received);
      fprintf(stderr, "decoded   : %u\n", st.decoded);
      fprintf(stderr, "decFailed : %u\n", st.decodeFailed);
    } else {
      fprintf(stderr, "captured  : %u\n", st.captured);
      fprintf(stderr, "encoded   : %u\n", st.encoded);
      fprintf(stderr, "sent      : %u\n", st.sent);
    }
    for (const QString& f : failures) {
      fprintf(stderr, "failure   : %s\n", qUtf8Printable(f));
    }
    const bool pass = connected && failures.isEmpty() &&
                      (role == QLatin1String("viewer")
                           ? st.decoded > 0
                           : st.sent > 0);
    fprintf(stderr, "RESULT    : %s\n", pass ? "PASS" : "FAIL");
    fflush(stderr);
    ::_exit(pass ? 0 : 1);
  });

  return app.exec();
}
