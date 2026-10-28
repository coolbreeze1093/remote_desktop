#include "ui/viewer_window.h"

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
  // 弹窗工具栏的画质档位 → 会话（控制通道下发到被控端编码器）。
  connect(videoWindow_, &RemoteVideoWindow::qualityModeSelected, this,
          [this](int mode) { session_->setVideoQualityMode(mode); });
  // rd_viewer 的共享屏幕由被控端选定，弹窗内不显示屏幕切换。
  videoWindow_->setScreenSelectorVisible(false);

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
  // 引擎随会话重建，档位回到默认：重放弹窗里用户选的档位。
  session_->setVideoQualityMode(videoWindow_->currentQualityMode());
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
