// remote_desktop — 直连信令：用一条 TCP 连接自动交换 Offer/Answer SDP。
//
// 服务端监听端口，客户端连上后发第一帧（Offer），服务端回一帧（Answer）。
// 信令 TCP 在 Answer 之后保持存活，供客户端主动断开时发送 BYE 帧通知
// 服务端停止推流。媒体与控制在 DataChannel 上点对点进行。
//
// 帧格式（全部小端）：
//   0..3  magic 'R','D','C','1'
//   4..7  payload 长度 u32 LE（上限 128 KiB，SDP 实际只有几 KiB）
//   8..   payload（UTF-8 SDP 文本）
//
// 帧类型约定：
//   - Offer  ：payload 为 SDP 文本（客户端第一帧）
//   - Answer ：payload 为 SDP 文本（服务端唯一回复帧）
//   - BYE    ：payload = "BYE"（客户端主动断开通知，服务端收到后停止推流）
#ifndef REMOTE_DESKTOP_SESSION_DIRECT_SIGNALING_H_
#define REMOTE_DESKTOP_SESSION_DIRECT_SIGNALING_H_

#include <QByteArray>
#include <QObject>
#include <QString>

class QTcpServer;
class QTcpSocket;

namespace rd {

// 被控端：监听 TCP 端口，收 Offer、发 Answer。同一时刻只服务一个客户端；
// 占线期间新到的连接立即关闭。
class DirectSignalingServer : public QObject {
  Q_OBJECT
 public:
  explicit DirectSignalingServer(QObject* parent = nullptr);
  ~DirectSignalingServer() override;

  bool listen(quint16 port);  // false 时用 lastError() 看原因
  void stopListen();
  bool isBusy() const { return client_ != nullptr; }
  // 给当前客户端回 Answer；TCP 保持存活等客户端发 BYE 或断连。
  void sendAnswer(const QString& sdpAnswer);
  // 不回 Answer 直接断开当前客户端（服务端忙/出错时快速失败）：
  // 本端发 clientLeft，对端按“回 Answer 前断开”处理。
  void rejectClient();
  QString lastError() const { return lastError_; }

 signals:
  void clientArrived(const QString& peer);
  void offerReceived(const QString& sdpOffer);
  // 客户端在交换完成前断开（发送方主动 close 或网络断）。
  void clientLeft();
  void listenFailed(const QString& reason);

 private:
  void onNewConnection();
  void onClientReadyRead();
  void onClientDisconnected();
  void resetClient();

  QTcpServer* server_ = nullptr;
  QTcpSocket* client_ = nullptr;
  QByteArray buf_;
  QString lastError_;
  bool offerTaken_ = false;  // 一次连接只收一帧 Offer
};

// 主控端：连到 host:port，发 Offer、收 Answer。
class DirectSignalingClient : public QObject {
  Q_OBJECT
 public:
  explicit DirectSignalingClient(QObject* parent = nullptr);
  ~DirectSignalingClient() override;

  void connectToHost(const QString& host, quint16 port);
  // socketConnected() 之后调用；一次连接只发这一帧。
  void sendOffer(const QString& sdpOffer);
  // 发送 BYE 帧通知服务端主动断开（在 abort 之前调用）。
  void sendBye();
  void abort();

 signals:
  void socketConnected();
  void answerReceived(const QString& sdpAnswer);
  // 连不上/超时/协议错/对端提前断开（区别于正常流程的 peerGone）。
  void failed(const QString& reason);
  // Answer 已收到、对端按约定关闭连接（正常流程的收尾）。
  void peerGone();

 private:
  void onConnected();
  void onReadyRead();
  void onDisconnected();
  void onError(int socketError);
  void fail(const QString& reason);

  QTcpSocket* socket_ = nullptr;
  QByteArray buf_;
  bool gotAnswer_ = false;
  bool aborting_ = false;  // 主动 abort 时吞掉 error/disconnected 信号
};

}  // namespace rd

#endif  // REMOTE_DESKTOP_SESSION_DIRECT_SIGNALING_H_