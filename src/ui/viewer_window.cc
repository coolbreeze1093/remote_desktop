#include "ui/viewer_window.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/input_event.h"
#include "ui/remote_video_window.h"

namespace rd {

ViewerWindow::ViewerWindow(QWidget* parent) : SessionWindow(parent) {
  // Independent fullscreen-capable window showing the remote video.
  videoWindow_ = new RemoteVideoWindow(this);
  // Frames arrive on libdatachannel threads; queue them to the video window.
  connect(session_, &RemoteDesktopSession::frameReady, videoWindow_,
          &RemoteVideoWindow::setFrame, Qt::QueuedConnection);
  // 远端光标本地渲染（宿主不再把光标烤进视频）。
  connect(session_, &RemoteDesktopSession::cursorMoved, videoWindow_,
          &RemoteVideoWindow::setRemoteCursorPos, Qt::QueuedConnection);
  connect(session_, &RemoteDesktopSession::cursorShapeChanged, videoWindow_,
          &RemoteVideoWindow::setRemoteCursorShape, Qt::QueuedConnection);
  connect(videoWindow_, &RemoteVideoWindow::disconnectRequested, this, [this] {
    if (session_->role() != RemoteDesktopSession::Role::None) {
      session_->stop();
      setStatus(tr("已断开连接"));
    }
  });
  // Forward mouse/keyboard on the remote video to the host.
  connect(videoWindow_, &RemoteVideoWindow::inputEvent, this,
          &ViewerWindow::sendInputEvent);

  // 宿主锁屏状态（引擎派发线程触发）：操作者在主控端看到的反馈。
  connect(session_, &RemoteDesktopSession::peerSecureDesktopChanged, this,
          [this](bool secure, bool capturable) {
            if (!secure) {
              viewerLockHint_->hide();
              return;
            }
            if (capturable) {
              viewerLockHint_->setText(
                  tr("被控端已锁屏 — 正在捕获锁屏画面，输入已切到安全桌面"));
              viewerLockHint_->setStyleSheet("color: #2e7d32;");
            } else {
              viewerLockHint_->setText(
                  tr("被控端已锁屏 — 被控端未以管理员运行，无法捕获/控制"
                     "锁屏；解锁后自动恢复"));
              viewerLockHint_->setStyleSheet("color: #c62828;");
            }
            viewerLockHint_->show();
          });

  setCentralWidget(makeViewerTab());
  // 标题带构建时间：部署副本（publish/）与构建目录的 exe 容易混跑，
  // 版本一眼可辨，避免拿旧版测出新问题。
  setWindowTitle(tr("远程桌面 主控端 — 手动 SDP 交换 (build %1 %2)").arg(
      QStringLiteral(__DATE__), QStringLiteral(__TIME__)));
  resize(960, 720);
  setStatus(tr("就绪"));
}

void ViewerWindow::onSessionConnected() {
  setStatus(tr("已连接 — 远端画面已弹出"));
  // 连接建立即弹出独立远端画面窗口（工具栏可退出全屏）。
  videoWindow_->showFullscreen();
  viewerShowVideoBtn_->show();
}

void ViewerWindow::onSessionDisconnected() {
  videoWindow_->close();
  viewerShowVideoBtn_->hide();
}

void ViewerWindow::applyDiagText(const QString& text) {
  videoWindow_->setDiagText(text);
}

QWidget* ViewerWindow::makeViewerTab() {
  auto* panel = new QWidget();
  auto* lay = new QVBoxLayout(panel);

  // Quality selector: 0=auto (adaptive ladder), 1=超清(native), 2=高清,
  // 3=流畅. Applied on the host encoder; sent over the control channel.
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

  stateLabel_ =
      new QLabel(tr("1) 创建 Offer 发给被控端，2) 粘贴 Answer 并连接"), panel);
  lay->addWidget(stateLabel_);

  // 连接中重新弹出远端画面窗口的入口（弹窗被关掉时从这里找回）。
  viewerShowVideoBtn_ = new QPushButton(tr("显示远端画面"), panel);
  viewerShowVideoBtn_->hide();
  connect(viewerShowVideoBtn_, &QPushButton::clicked, videoWindow_,
          &RemoteVideoWindow::showFullscreen);
  lay->addWidget(viewerShowVideoBtn_);

  // 宿主侧锁屏/UAC 状态提示（锁屏期间画面冻结时的原因说明）。
  viewerLockHint_ = new QLabel(panel);
  viewerLockHint_->setWordWrap(true);
  viewerLockHint_->hide();
  lay->addWidget(viewerLockHint_);

  lay->addWidget(new QLabel(tr("你的 Offer（复制发给被控端）:"), panel));
  viewerOffer_ = new QPlainTextEdit(panel);
  viewerOffer_->setReadOnly(true);
  lay->addWidget(viewerOffer_, 2);

  auto* btnRow = new QHBoxLayout();
  viewerOfferBtn_ = new QPushButton(tr("创建 Offer"), panel);
  viewerConnectBtn_ = new QPushButton(tr("粘贴 Answer 并连接"), panel);
  viewerConnectBtn_->setEnabled(false);
  btnRow->addWidget(viewerOfferBtn_);
  btnRow->addWidget(viewerConnectBtn_);
  lay->addLayout(btnRow);

  lay->addWidget(new QLabel(tr("粘贴被控端的 Answer:"), panel));
  viewerAnswer_ = new QPlainTextEdit(panel);
  viewerAnswer_->setPlaceholderText(tr("在此粘贴 Answer SDP..."));
  lay->addWidget(viewerAnswer_, 2);

  connect(viewerOfferBtn_, &QPushButton::clicked, this,
          &ViewerWindow::onViewerCreateOffer);
  connect(viewerConnectBtn_, &QPushButton::clicked, this,
          &ViewerWindow::onViewerConnect);

  return panel;
}

void ViewerWindow::sendInputEvent(const rd::InputEvent& ev) {
  session_->sendInput(ev);
}

void ViewerWindow::onViewerCreateOffer() {
  if (!session_->startViewer()) {
    QMessageBox::warning(this, tr("远程桌面"), tr("初始化失败"));
    return;
  }
  const QString offer = session_->viewerCreateOffer();
  if (offer.isEmpty()) {
    // Offer creation failed (e.g. ICE gathering timeout) — reset for retry.
    session_->stop();
    QMessageBox::warning(this, tr("远程桌面"), tr("创建 Offer 失败，请检查网络"));
    return;
  }
  viewerOffer_->setPlainText(offer);
  viewerConnectBtn_->setEnabled(true);
  setStatus(tr("Offer 已创建 — 复制给被控端，等它回 Answer"));
}

void ViewerWindow::onViewerConnect() {
  const QString answer = viewerAnswer_->toPlainText().trimmed();
  if (answer.isEmpty()) {
    QMessageBox::information(this, tr("远程桌面"), tr("请先粘贴被控端的 Answer"));
    return;
  }
  if (!session_->viewerAcceptAnswer(answer)) {
    QMessageBox::warning(this, tr("远程桌面"), tr("Answer 无效"));
    return;
  }
  setStatus(tr("正在连接..."));
}

}  // namespace rd
