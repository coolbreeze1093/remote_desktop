#include "session/direct_signaling.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QNetworkProxy>

namespace rd {

namespace {

constexpr char kMagic[4] = {'R', 'D', 'C', '1'};
constexpr quint32 kMaxPayload = 128 * 1024;
// 覆盖 连接 + 服务端生成 Answer（内含 ICE 收集，最长 ~10s）+ 余量。
constexpr int kSignalTimeoutMs = 25000;

void AppendFrame(QByteArray& out, const QByteArray& payload) {
  const auto len = static_cast<quint32>(payload.size());
  out.append(kMagic, static_cast<int>(sizeof(kMagic)));
  for (int i = 0; i < 4; ++i) {
    out.append(static_cast<char>((len >> (8 * i)) & 0xFF));
  }
  out.append(payload);
}

enum class FrameResult { kNeedMore, kOk, kBad };

FrameResult ExtractFrame(QByteArray& buf, QString& out) {
  if (buf.size() < 8) {
    return FrameResult::kNeedMore;
  }
  if (memcmp(buf.constData(), kMagic, sizeof(kMagic)) != 0) {
    return FrameResult::kBad;
  }
  quint32 len = 0;
  for (int i = 0; i < 4; ++i) {
    len |= static_cast<quint32>(static_cast<uchar>(buf[4 + i])) << (8 * i);
  }
  if (len > kMaxPayload) {
    return FrameResult::kBad;
  }
  if (static_cast<quint32>(buf.size() - 8) < len) {
    return FrameResult::kNeedMore;
  }
  out = QString::fromUtf8(buf.constData() + 8, static_cast<int>(len));
  buf.remove(0, 8 + static_cast<int>(len));
  return FrameResult::kOk;
}

}  // namespace

// ---------------------------------------------------------------- 服务端

DirectSignalingServer::DirectSignalingServer(QObject* parent)
    : QObject(parent) {
  server_ = new QTcpServer(this);
  connect(server_, &QTcpServer::newConnection, this,
          &DirectSignalingServer::onNewConnection);
}

DirectSignalingServer::~DirectSignalingServer() { stopListen(); }

bool DirectSignalingServer::listen(quint16 port) {
  stopListen();
  if (!server_->listen(QHostAddress::Any, port)) {
    lastError_ = server_->errorString();
    emit listenFailed(lastError_);
    return false;
  }
  lastError_.clear();
  return true;
}

void DirectSignalingServer::stopListen() {
  resetClient();
  if (server_->isListening()) {
    server_->close();
  }
}

void DirectSignalingServer::onNewConnection() {
  while (QTcpSocket* next = server_->nextPendingConnection()) {
    if (client_ != nullptr) {
      // 占线：只服务一个客户端，其余立即断开。
      next->abort();
      next->deleteLater();
      continue;
    }
    client_ = next;
    buf_.clear();
    offerTaken_ = false;
    connect(client_, &QTcpSocket::disconnected, this,
            &DirectSignalingServer::onClientDisconnected);
    connect(client_, &QTcpSocket::readyRead, this,
            &DirectSignalingServer::onClientReadyRead);
    emit clientArrived(client_->peerAddress().toString() + ":" +
                       QString::number(client_->peerPort()));
  }
}

void DirectSignalingServer::onClientReadyRead() {
  if (client_ == nullptr) {
    return;
  }
  buf_ += client_->readAll();
  if (!offerTaken_) {
    // 还未收到 Offer：提取第一帧作为 Offer。
    QString payload;
    switch (ExtractFrame(buf_, payload)) {
      case FrameResult::kNeedMore:
        return;
      case FrameResult::kBad:
        resetClient();
        return;
      case FrameResult::kOk:
        offerTaken_ = true;
        emit offerReceived(payload);
        // 继续检查缓冲区中是否还有后续帧（含 BYE）。
        break;
    }
  }
  // offerTaken_ 之后：只需识别 BYE 帧。
  while (offerTaken_ && client_ != nullptr &&
         static_cast<quint32>(buf_.size()) >= 8) {
    if (memcmp(buf_.constData(), kMagic, sizeof(kMagic)) != 0) {
      resetClient();
      return;
    }
    quint32 len = 0;
    for (int i = 0; i < 4; ++i) {
      len |= static_cast<quint32>(static_cast<uchar>(buf_[4 + i])) << (8 * i);
    }
    if (len > kMaxPayload) {
      resetClient();
      return;
    }
    if (static_cast<quint32>(buf_.size() - 8) < len) {
      return;  // 帧数据不全，等更多数据
    }
    const QString payload =
        QString::fromUtf8(buf_.constData() + 8, static_cast<int>(len));
    buf_.remove(0, 8 + static_cast<int>(len));
    if (payload == QStringLiteral("BYE")) {
      const bool hadClient = client_ != nullptr;
      resetClient();
      if (hadClient) {
        emit clientLeft();
      }
      return;
    }
    // 非 BYE 帧：忽略，继续检查下一帧。
  }
}

void DirectSignalingServer::sendAnswer(const QString& sdpAnswer) {
  if (client_ == nullptr) {
    return;
  }
  QByteArray frame;
  AppendFrame(frame, sdpAnswer.toUtf8());
  client_->write(frame);
  client_->flush();
  // TCP 保持存活：客户端收到 Answer 后开始 P2P，断开时发 BYE 帧通知本端。
}

void DirectSignalingServer::rejectClient() {
  if (client_ == nullptr) {
    return;
  }
  resetClient();
  emit clientLeft();
}

void DirectSignalingServer::onClientDisconnected() {
  const bool hadClient = client_ != nullptr;
  resetClient();
  if (hadClient) {
    emit clientLeft();
  }
}

void DirectSignalingServer::resetClient() {
  buf_.clear();
  offerTaken_ = false;
  if (client_ != nullptr) {
    QTcpSocket* dying = client_;
    client_ = nullptr;
    dying->abort();
    dying->deleteLater();
  }
}

// ---------------------------------------------------------------- 客户端

DirectSignalingClient::DirectSignalingClient(QObject* parent)
    : QObject(parent) {
  socket_ = new QTcpSocket(this);
  socket_->setProxy(QNetworkProxy::NoProxy);
  connect(socket_, &QTcpSocket::connected, this,
          &DirectSignalingClient::onConnected);
  connect(socket_, &QTcpSocket::readyRead, this,
          &DirectSignalingClient::onReadyRead);
  connect(socket_, &QTcpSocket::disconnected, this,
          &DirectSignalingClient::onDisconnected);
  connect(socket_,
          QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
          this, &DirectSignalingClient::onError);
}

DirectSignalingClient::~DirectSignalingClient() { abort(); }

void DirectSignalingClient::connectToHost(const QString& host, quint16 port) {
  abort();
  aborting_ = false;
  gotAnswer_ = false;
  buf_.clear();
  // Qt 5.12 没有 setTransferTimeout：用定时器兜底整个信令交换时长
  // （连接 + 服务端生成 Answer 内含的 ICE 收集）。收到 Answer 即解除。
  QTimer::singleShot(kSignalTimeoutMs, this, [this] {
    if (!gotAnswer_) {
      fail(QStringLiteral("信令超时（未在时限内收到 Answer）"));
    }
  });
  socket_->connectToHost(host, port);
}

void DirectSignalingClient::onConnected() { emit socketConnected(); }

void DirectSignalingClient::sendOffer(const QString& sdpOffer) {
  QByteArray frame;
  AppendFrame(frame, sdpOffer.toUtf8());
  socket_->write(frame);
  socket_->flush();
}

void DirectSignalingClient::sendBye() {
  QByteArray frame;
  AppendFrame(frame, QByteArray("BYE", 3));
  socket_->write(frame);
  socket_->flush();
}

void DirectSignalingClient::onReadyRead() {
  buf_ += socket_->readAll();
  QString payload;
  switch (ExtractFrame(buf_, payload)) {
    case FrameResult::kNeedMore:
      return;
    case FrameResult::kBad:
      fail(QStringLiteral("对端不是远程桌面直连服务端（协议不匹配）"));
      return;
    case FrameResult::kOk:
      gotAnswer_ = true;
      emit answerReceived(payload);
      return;
  }
}

void DirectSignalingClient::onDisconnected() {
  if (aborting_) {
    return;
  }
  if (gotAnswer_) {
    emit peerGone();
  } else {
    fail(QStringLiteral("对端在回 Answer 之前断开（服务端忙或拒绝）"));
  }
}

void DirectSignalingClient::onError(int socketError) {
  if (aborting_) {
    return;
  }
  Q_UNUSED(socketError);
  if (socket_->state() != QAbstractSocket::ConnectedState && !gotAnswer_) {
    fail(socket_->errorString());
  }
}

void DirectSignalingClient::fail(const QString& reason) {
  aborting_ = true;
  socket_->abort();
  emit failed(reason);
}

void DirectSignalingClient::abort() {
  aborting_ = true;
  gotAnswer_ = false;
  buf_.clear();
  socket_->abort();
}

}  // namespace rd