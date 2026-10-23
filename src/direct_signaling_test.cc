// rd_direct_signaling_test — 直连信令层无头自测（控制台，QCoreApplication）。
// 验证：端口监听、帧编解码、Offer/Answer 往返、服务端回完 Answer 主动关连接、
// 客户端把对端关闭识别为正常收尾（peerGone 而非 failed）。
#include <QCoreApplication>
#include <QTimer>

#include <cstdio>

#include "session/direct_signaling.h"

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  int failures = 0;
  auto check = [&failures](bool ok, const char* what) {
    if (!ok) {
      std::fprintf(stderr, "FAIL: %s\n", what);
      ++failures;
    }
  };

  rd::DirectSignalingServer server;
  // 测试专用端口；被占用时换下一个（跳过固定端口的偶发冲突）。
  quint16 port = 48511;
  while (!server.listen(port)) {
    ++port;
    if (port > 48600) {
      std::fprintf(stderr, "FAIL: no free port\n");
      return 1;
    }
  }

  QString gotOffer, gotAnswer, sigFail;
  bool peerGone = false;
  QObject::connect(&server, &rd::DirectSignalingServer::offerReceived,
                   &app, [&](const QString& offer) {
                     gotOffer = offer;
                     server.sendAnswer(QStringLiteral("ANSWER-SDP-123"));
                   });
  rd::DirectSignalingClient client;
  QObject::connect(&client, &rd::DirectSignalingClient::socketConnected,
                   &app, [&] { client.sendOffer("OFFER-SDP-ABC"); });
  QObject::connect(&client, &rd::DirectSignalingClient::answerReceived,
                   &app, [&](const QString& answer) { gotAnswer = answer; });
  QObject::connect(&client, &rd::DirectSignalingClient::peerGone, &app,
                   [&] { peerGone = true; QTimer::singleShot(50, &app, &QCoreApplication::quit); });
  QObject::connect(&client, &rd::DirectSignalingClient::failed, &app,
                   [&](const QString& reason) {
                     sigFail = reason;
                     QTimer::singleShot(0, &app, &QCoreApplication::quit);
                   });

  client.connectToHost(QStringLiteral("127.0.0.1"), port);
  QTimer::singleShot(15000, &app, &QCoreApplication::quit);  // 兜底超时
  app.exec();

  check(sigFail.isEmpty(), "no signaling failure");
  check(gotOffer == "OFFER-SDP-ABC", "offer roundtrip");
  check(gotAnswer == "ANSWER-SDP-123", "answer roundtrip");
  check(peerGone, "server close after answer -> peerGone");

  if (failures == 0) {
    std::fprintf(stderr, "rd_direct_signaling_test: ALL PASS\n");
    return 0;
  }
  std::fprintf(stderr, "rd_direct_signaling_test: %d FAILURES\n", failures);
  return 1;
}
