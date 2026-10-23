#include "ui/direct_viewer_window.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "../debug_log.h"
#include "core/input_event.h"
#include "session/direct_signaling.h"
#include "ui/remote_video_window.h"
#include "ui/video_widget.h"

namespace rd {

namespace {
constexpr quint16 kDefaultPort = 48010;
}  // namespace

DirectViewerWindow::DirectViewerWindow(QWidget* parent)
    : SessionWindow(parent) {
  setWindowTitle(tr("远程桌面 直连主控端 — 输入 IP 直连"));
  resize(960, 720);
  setCentralWidget(makeViewerPanel());

  signaling_ = new DirectSignalingClient(this);
  connect(signaling_, &DirectSignalingClient::socketConnected, this, [this] {
    rdDebug("direct-viewer", "signaling connected, sending offer len=%d",
            pendingOffer_.size());
    setStatus(tr("已连上服务端，交换信令中..."));
    signaling_->sendOffer(pendingOffer_);
  });
  connect(signaling_, &DirectSignalingClient::answerReceived, this,
          &DirectViewerWindow::onAnswerReceived);
  connect(signaling_, &DirectSignalingClient::failed, this,
          &DirectViewerWindow::onSignalingFailed);
  // 正常收尾（对端按约定关闭信令连接）：无事可做。

  setStatusBar(nullptr);
  setStatus(tr("就绪 — 填写被控端 IP 与端口后点\"连接\""));
}

QWidget* DirectViewerWindow::makeViewerPanel() {
  auto* panel = new QWidget();
  auto* lay = new QVBoxLayout(panel);

  // 与 rd_viewer 相同的全屏远端窗口：帧排队进窗口、键鼠转发、断开按钮。
  videoWindow_ = new RemoteVideoWindow(this);
  connect(session_, &RemoteDesktopSession::frameReady, videoWindow_,
          &RemoteVideoWindow::setFrame, Qt::QueuedConnection);
  connect(videoWindow_, &RemoteVideoWindow::inputEvent, this,
          &DirectViewerWindow::sendInputEvent);
  connect(videoWindow_, &RemoteVideoWindow::disconnectRequested, this, [this] {
    if (session_->role() != RemoteDesktopSession::Role::None) {
      signaling_->sendBye();
      session_->stop();
      signaling_->abort();
      resetForRetry(tr("已断开连接"));
    }
  });

  // 连接行：IP + 端口 + 连接按钮。
  auto* connRow = new QHBoxLayout();
  connRow->addWidget(new QLabel(tr("被控端 IP:"), panel));
  hostEdit_ = new QLineEdit(panel);
  hostEdit_->setPlaceholderText(tr("例如 192.168.1.20"));
  connRow->addWidget(hostEdit_, 2);
  connRow->addWidget(new QLabel(tr("端口:"), panel));
  portEdit_ = new QLineEdit(QString::number(kDefaultPort), panel);
  connRow->addWidget(portEdit_, 1);
  connectBtn_ = new QPushButton(tr("连接"), panel);
  connect(connectBtn_, &QPushButton::clicked, this,
          &DirectViewerWindow::onToggleConnect);
  connRow->addWidget(connectBtn_);
  lay->addLayout(connRow);

  // 画质选择：0=auto（自适应阶梯），1=超清 2=高清 3=流畅。
  auto* qualityRow = new QHBoxLayout();
  qualityRow->addWidget(new QLabel(tr("画质:"), panel));
  qualityBox_ = new QComboBox(panel);
  qualityBox_->addItem(tr("自动（根据网络和负载调节）"), 0);
  qualityBox_->addItem(tr("超清（原生分辨率）"), 1);
  qualityBox_->addItem(tr("高清"), 2);
  qualityBox_->addItem(tr("流畅"), 3);
  connect(qualityBox_, qOverload<int>(&QComboBox::activated), this,
          [this](int index) {
            session_->setVideoQualityMode(qualityBox_->itemData(index).toInt());
          });
  qualityRow->addWidget(qualityBox_, 1);
  lay->addLayout(qualityRow);

  stateLabel_ = new QLabel(panel);
  stateLabel_->setWordWrap(true);
  lay->addWidget(stateLabel_);

  lockHint_ = new QLabel(panel);
  lockHint_->setWordWrap(true);
  lockHint_->hide();
  lay->addWidget(lockHint_);

  // 内嵌实时预览（与 rd_viewer 相同）：同机测试不依赖全屏弹窗。
  viewerPreview_ = new VideoWidget;
  viewerPreview_->setToolTip(tr("双击打开/关闭全屏远端画面窗口"));
  connect(viewerPreview_, &VideoWidget::inputEvent, this,
          &DirectViewerWindow::sendInputEvent);
  // 补上与提示一致的交互：双击预览切换全屏远端窗口。
  connect(viewerPreview_, &VideoWidget::doubleClicked, videoWindow_,
          &RemoteVideoWindow::toggleFullscreen);
  connect(session_, &RemoteDesktopSession::frameReady, viewerPreview_,
          &VideoWidget::setFrame, Qt::QueuedConnection);
  lay->addWidget(viewerPreview_, 1);
  viewerPreview_->hide();

  // 被控端锁屏状态提示（引擎派发线程触发）。
  connect(session_, &RemoteDesktopSession::peerSecureDesktopChanged, this,
          [this](bool secure, bool capturable) {
            if (!secure) {
              lockHint_->hide();
              return;
            }
            if (capturable) {
              lockHint_->setText(
                  tr("被控端已锁屏 — 正在捕获锁屏画面，输入已切到安全桌面"));
              lockHint_->setStyleSheet("color: #2e7d32;");
            } else {
              lockHint_->setText(
                  tr("被控端已锁屏 — 被控端未以管理员运行，无法捕获/控制"
                     "锁屏；解锁后自动恢复"));
              lockHint_->setStyleSheet("color: #c62828;");
            }
            lockHint_->show();
          });

  return panel;
}

void DirectViewerWindow::autoConnect(const QString& host, quint16 port) {
  hostEdit_->setText(host);
  portEdit_->setText(QString::number(port));
  onToggleConnect();
}

void DirectViewerWindow::onToggleConnect() {
  if (session_->role() != RemoteDesktopSession::Role::None ||
      connecting_) {
    // 已连接/连接中：当作断开处理。
    signaling_->sendBye();
    session_->stop();
    signaling_->abort();
    resetForRetry(tr("已断开连接"));
    return;
  }
  const QString host = hostEdit_->text().trimmed();
  bool ok = false;
  const quint16 port = portEdit_->text().toUShort(&ok);
  if (host.isEmpty() || !ok || port == 0) {
    QMessageBox::information(this, tr("远程桌面"),
                             tr("请填写被控端 IP 与有效端口"));
    return;
  }
  resetForRetry(tr("初始化..."));
  if (!session_->startViewer()) {
    QMessageBox::warning(this, tr("远程桌面"), tr("初始化失败"));
    resetForRetry(tr("就绪"));
    return;
  }
  pendingOffer_ = session_->viewerCreateOffer();
  if (pendingOffer_.isEmpty()) {
    session_->stop();
    QMessageBox::warning(this, tr("远程桌面"), tr("创建 Offer 失败，请检查网络"));
    resetForRetry(tr("就绪"));
    return;
  }
  connecting_ = true;
  connectBtn_->setText(tr("取消"));
  rdDebug("direct-viewer", "connect to %s:%u offer len=%d",
          host.toUtf8().constData(), (unsigned)port, pendingOffer_.size());
  setStatus(tr("连接 %1:%2 ...（Offer 已就绪，等待信令）").arg(host).arg(port));
  signaling_->connectToHost(host, port);
}

void DirectViewerWindow::onAnswerReceived(const QString& answer) {
  rdDebug("direct-viewer", "answer received len=%d", answer.size());
  if (!session_->viewerAcceptAnswer(answer)) {
    QMessageBox::warning(this, tr("远程桌面"), tr("Answer 无效"));
    session_->stop();
    resetForRetry(tr("就绪"));
    return;
  }
  connecting_ = false;
  connectBtn_->setText(tr("断开"));
  setStatus(tr("Answer 已收到，P2P 建立中..."));
  viewerPreview_->show();
}

void DirectViewerWindow::onSignalingFailed(const QString& reason) {
  if (!connecting_) {
    return;  // 已进入 P2P/已连接阶段的断开由 session 回调处理。
  }
  rdDebug("direct-viewer", "signaling failed: %s", reason.toUtf8().constData());
  session_->stop();
  resetForRetry(tr("连接失败: %1").arg(reason));
}

void DirectViewerWindow::sendInputEvent(const rd::InputEvent& ev) {
  session_->sendInput(ev);
}

void DirectViewerWindow::resetForRetry(const QString& statusText) {
  connecting_ = false;
  pendingOffer_.clear();
  connectBtn_->setText(tr("连接"));
  connectBtn_->setEnabled(true);
  setStatus(statusText);
}

void DirectViewerWindow::onSessionConnected() {
  rdDebug("direct-viewer", "session connected");
  setStatus(tr("已连接 — 双击预览画面可全屏"));
}

void DirectViewerWindow::onSessionDisconnected() {
  rdDebug("direct-viewer", "session disconnected");
  signaling_->abort();
  videoWindow_->close();
  resetForRetry(tr("连接断开，可重新连接"));
}

void DirectViewerWindow::closeEvent(QCloseEvent* event) {
  if (session_->role() != RemoteDesktopSession::Role::None || connecting_) {
    signaling_->sendBye();
    session_->stop();
    signaling_->abort();
  }
  event->accept();
}

}  // namespace rd