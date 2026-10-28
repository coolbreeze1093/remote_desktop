#include "ui/session_window.h"

#include <QLabel>
#include <QMessageBox>
#include <QStatusBar>
#include <QTimer>

namespace rd {

SessionWindow::SessionWindow(QWidget* parent) : QMainWindow(parent) {
  session_ = new RemoteDesktopSession(this);
  connect(session_, &RemoteDesktopSession::failed, this,
          [this](const QString& r) { onSessionFailed(r); });
  connect(session_, &RemoteDesktopSession::connected, this,
          [this] { onSessionConnected(); });
  connect(session_, &RemoteDesktopSession::disconnected, this, [this] {
    setStatus(tr("连接断开"));
    onSessionDisconnected();
  });

  // Poll pipeline counters once a second: they localise a "connected but no
  // picture" report to the stage that stopped (capture/encode/send vs
  // receive/decode/display).
  diagTimer_ = new QTimer(this);
  connect(diagTimer_, &QTimer::timeout, this, &SessionWindow::updateDiag);
  diagTimer_->start(1000);
}

void SessionWindow::onSessionConnected() {
  setStatus(tr("已连接"));
}

void SessionWindow::onSessionDisconnected() {}

void SessionWindow::onSessionFailed(const QString& reason) {
  setStatus(tr("错误: %1").arg(reason));
  QMessageBox::warning(this, tr("远程桌面"), reason);
}

void SessionWindow::setStatus(const QString& text) {
  statusBar()->showMessage(text);
  if (stateLabel_) {
    stateLabel_->setText(text);
  }
}

void SessionWindow::updateDiag() {
  const auto role = session_->role();
  if (role == RemoteDesktopSession::Role::None) {
    return;
  }
  const auto st = session_->diagStats();
  static const char* kLevelNames[] = {"流畅", "高清", "超清"};
  QString text;
  if (role == RemoteDesktopSession::Role::Host) {
    text = tr("管道: 捕获 %1 / 编码 %2 / 已发送 %3 / 丢弃 %4")
               .arg(st.captured)
               .arg(st.encoded)
               .arg(st.sent)
               .arg(st.dropped);
    if (st.qualityLevel >= 0 && st.qualityLevel <= 2 && st.encodeWidth > 0) {
      static const int kMbps[] = {4, 12, 24};
      text += tr(" | 档位: %1 %2×%3 ~%4Mbps 编码%5ms")
                  .arg(QString::fromUtf8(kLevelNames[st.qualityLevel]))
                  .arg(st.encodeWidth)
                  .arg(st.encodeHeight)
                  .arg(kMbps[st.qualityLevel])
                  .arg(st.encodeMs);
    }
  } else {
    text = tr("管道: 收到 %1 帧 / 解码 %2 / 解码失败 %3")
               .arg(st.received)
               .arg(st.decoded)
               .arg(st.decodeFailed);
    if (st.encodeWidth > 0) {
      text += tr(" | 对端编码 %1×%2").arg(st.encodeWidth).arg(st.encodeHeight);
    }
  }
  if (stateLabel_) {
    stateLabel_->setText(text);
  }
  applyDiagText(text);
}

}  // namespace rd
