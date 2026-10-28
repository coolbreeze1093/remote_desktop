#include "ui/direct_viewer_window.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "../debug_log.h"
#include "core/input_event.h"
#include "session/direct_signaling.h"
#include "ui/remote_video_window.h"

namespace rd {

namespace {
constexpr quint16 kDefaultPort = 48010;
// 自动重连退避：被控端解锁/会话切换会拆掉 helper 连同 WebRTC 会话。信令
// 断开即触发重连（见构造函数 peerGone 接线），服务端监督者 ≤2s 重拉 helper、
// relay 押住 Offer（最长 20s），首试 1.5s 即可接上；后续逐次拉长兜住被控端
// 长时间离线的情况。
constexpr int kMaxAutoReconnects = 5;
const int kRetryDelaysMs[] = {1500, 3000, 5000, 8000, 12000};
}  // namespace

DirectViewerWindow::DirectViewerWindow(QWidget* parent)
    : SessionWindow(parent) {
  setWindowTitle(tr("远程桌面 直连主控端 — 输入 IP 直连"));
  resize(960, 720);
  setCentralWidget(makeViewerPanel());

  signaling_ = new DirectSignalingClient(this);
  connect(signaling_, &DirectSignalingClient::socketConnected, this, [this] {
    setStatus(tr("已连上服务端，获取屏幕列表..."));
    // 先查被控端屏幕列表（选屏），到齐或超时后再发 Offer；流程见 sendOfferNow。
    offerSent_ = false;
    signaling_->requestScreens();
    if (!screensTimer_) {
      screensTimer_ = new QTimer(this);
      screensTimer_->setSingleShot(true);
      connect(screensTimer_, &QTimer::timeout, this, [this] {
        if (!offerSent_) {
          rdDebug("direct-viewer", "screens list timeout — sending offer without screen");
          sendOfferNow();
        }
      });
    }
    screensTimer_->start(2000);
  });
  connect(signaling_, &DirectSignalingClient::screensReceived, this,
          &DirectViewerWindow::onScreensReceived);
  connect(signaling_, &DirectSignalingClient::answerReceived, this,
          &DirectViewerWindow::onAnswerReceived);
  connect(signaling_, &DirectSignalingClient::failed, this,
          &DirectViewerWindow::onSignalingFailed);
  // 推流中信令连接断开 = 被控端会话已终结（解锁/会话切换把 helper 连同
  // WebRTC 会话一起拆了，relay 主动关 socket）。不等 ICE 自己超时发现对端
  // 消失（实测 ~8s 才宣告断开），立刻拆本地会话进入自动重连；重连的 Offer
  // 会被 relay 押住等新 helper 就绪（最长 20s），恢复窗口压到 ~3s。
  connect(signaling_, &DirectSignalingClient::peerGone, this, [this] {
    if (userStopped_ || !everConnected_ ||
        session_->role() == RemoteDesktopSession::Role::None) {
      return;  // Offer 阶段的断开走 failed；主动断开/已收尾不重复处理
    }
    rdDebug("direct-viewer",
            "signaling lost mid-session — teardown now for fast reconnect");
    session_->stop();
    onSessionDisconnected();
  });

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
  // 远端光标本地渲染（宿主不再把光标烤进视频）。
  connect(session_, &RemoteDesktopSession::cursorMoved, videoWindow_,
          &RemoteVideoWindow::setRemoteCursorPos, Qt::QueuedConnection);
  connect(session_, &RemoteDesktopSession::cursorShapeChanged, videoWindow_,
          &RemoteVideoWindow::setRemoteCursorShape, Qt::QueuedConnection);
  connect(videoWindow_, &RemoteVideoWindow::inputEvent, this,
          &DirectViewerWindow::sendInputEvent);
  connect(videoWindow_, &RemoteVideoWindow::disconnectRequested, this, [this] {
    if (session_->role() != RemoteDesktopSession::Role::None) {
      userStopped_ = true;
      reconnectTimer_->stop();
      signaling_->sendBye();
      session_->stop();
      signaling_->abort();
      resetForRetry(tr("已断开连接"));
    }
  });

  // 异常断开自动重连定时器（用户主动断开会置 userStopped_ 并停表）。
  reconnectTimer_ = new QTimer(this);
  reconnectTimer_->setSingleShot(true);
  connect(reconnectTimer_, &QTimer::timeout, this, [this] {
    if (userStopped_ || autoPort_ == 0 ||
        session_->role() != RemoteDesktopSession::Role::None || connecting_) {
      return;
    }
    rdDebug("direct-viewer", "auto-reconnect attempt %d/%d to %s:%u",
            autoAttempts_ + 1, kMaxAutoReconnects,
            autoHost_.toUtf8().constData(), (unsigned)autoPort_);
    startConnection(autoHost_, autoPort_);
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

  // 被控端屏幕选择：连接时自动向被控端查询屏幕列表（服务模式由 relay 应答），
  // 选中项随 Offer 带给 helper；拉不到列表时按老格式发（= 被控端默认屏）。
  auto* screenRow = new QHBoxLayout();
  screenRow->addWidget(new QLabel(tr("被控端屏幕:"), panel));
  screenBox_ = new QComboBox(panel);
  screenBox_->addItem(tr("被控端默认（连接后自动获取列表）"), 0);
  screenBox_->setEnabled(false);
  screenBox_->setToolTip(tr("连接后自动获取被控端屏幕列表；重连沿用当前选择"));
  screenRow->addWidget(screenBox_, 1);
  lay->addLayout(screenRow);

  stateLabel_ = new QLabel(panel);
  stateLabel_->setWordWrap(true);
  lay->addWidget(stateLabel_);

  // 连接中重新弹出远端画面窗口的入口（弹窗被关掉时从这里找回）。
  showVideoBtn_ = new QPushButton(tr("显示远端画面"), panel);
  showVideoBtn_->hide();
  connect(showVideoBtn_, &QPushButton::clicked, videoWindow_,
          &RemoteVideoWindow::showFullscreen);
  lay->addWidget(showVideoBtn_);

  lockHint_ = new QLabel(panel);
  lockHint_->setWordWrap(true);
  lockHint_->hide();
  lay->addWidget(lockHint_);

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
    userStopped_ = true;
    reconnectTimer_->stop();
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
  startConnection(host, port);
}

bool DirectViewerWindow::startConnection(const QString& host, quint16 port) {
  // 自动重连会带着上一个会话的残留进来（引擎已 Closed 但未 stop）：先清理。
  reconnectTimer_->stop();
  userStopped_ = false;
  if (session_->role() != RemoteDesktopSession::Role::None || connecting_) {
    signaling_->sendBye();
    session_->stop();
    signaling_->abort();
  }
  resetForRetry(tr("初始化..."));
  if (!session_->startViewer()) {
    QMessageBox::warning(this, tr("远程桌面"), tr("初始化失败"));
    resetForRetry(tr("就绪"));
    return false;
  }
  pendingOffer_ = session_->viewerCreateOffer();
  if (pendingOffer_.isEmpty()) {
    session_->stop();
    QMessageBox::warning(this, tr("远程桌面"), tr("创建 Offer 失败，请检查网络"));
    resetForRetry(tr("就绪"));
    return false;
  }
  autoHost_ = host;
  autoPort_ = port;
  ++autoAttempts_;
  connecting_ = true;
  connectBtn_->setText(tr("取消"));
  rdDebug("direct-viewer", "connect to %s:%u offer len=%d",
          host.toUtf8().constData(), (unsigned)port, pendingOffer_.size());
  setStatus(tr("连接 %1:%2 ...（Offer 已就绪，等待信令）").arg(host).arg(port));
  signaling_->connectToHost(host, port);
  return true;
}

void DirectViewerWindow::scheduleAutoReconnect(const QString& reason) {
  if (userStopped_ || autoPort_ == 0 || !everConnected_ ||
      autoAttempts_ >= kMaxAutoReconnects) {
    resetForRetry(tr("%1，可重新连接").arg(reason));
    return;
  }
  const int delay = kRetryDelaysMs[std::min(autoAttempts_, kMaxAutoReconnects - 1)];
  setStatus(tr("%1 — %2 秒后自动重连（第 %3/%4 次）...")
                .arg(reason)
                .arg(delay / 1000)
                .arg(autoAttempts_ + 1)
                .arg(kMaxAutoReconnects));
  reconnectTimer_->start(delay);
}

void DirectViewerWindow::onScreensReceived(const QVector<qint64>& screenIds,
                                           const QStringList& screenTitles) {
  rdDebug("direct-viewer", "screens received: %lld",
          (long long)screenIds.size());
  screensKnown_ = true;
  screenIds_ = screenIds;
  const QString prev = screenBox_->currentText();
  screenBox_->blockSignals(true);
  screenBox_->clear();
  for (int i = 0; i < screenIds.size(); ++i) {
    QString name = i < screenTitles.size() ? screenTitles[i] : QString();
    if (name.isEmpty()) {
      name = tr("屏幕 %1").arg(i + 1);
    }
    screenBox_->addItem(name, QVariant::fromValue<qint64>(screenIds[i]));
  }
  // 重连沿用上次选择；首次默认第一个真实屏幕（避开 SCL_SCREEN_DEFAULT 在
  // 个别环境失效的问题）。
  int idx = screenBox_->findText(prev);
  if (idx < 0) {
    idx = 0;
  }
  screenBox_->setCurrentIndex(idx);
  screenBox_->blockSignals(false);
  screenBox_->setEnabled(true);
  if (!offerSent_) {
    sendOfferNow();
  }
}

void DirectViewerWindow::sendOfferNow() {
  if (offerSent_ || pendingOffer_.isEmpty()) {
    return;
  }
  offerSent_ = true;
  if (screensTimer_) {
    screensTimer_->stop();
  }
  if (screensKnown_ && screenBox_->currentIndex() >= 0 &&
      screenBox_->currentIndex() < screenIds_.size()) {
    const qint64 id = screenIds_[screenBox_->currentIndex()];
    rdDebug("direct-viewer",
            "signaling connected, sending offer len=%d screen=%lld",
            pendingOffer_.size(), (long long)id);
    setStatus(tr("已连上服务端，交换信令中（屏幕：%1）...")
                  .arg(screenBox_->currentText()));
    signaling_->sendOfferWithScreen(pendingOffer_, id);
  } else {
    rdDebug("direct-viewer", "signaling connected, sending offer len=%d (host default screen)",
            pendingOffer_.size());
    setStatus(tr("已连上服务端，交换信令中..."));
    signaling_->sendOffer(pendingOffer_);
  }
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
}

void DirectViewerWindow::onSignalingFailed(const QString& reason) {
  if (!connecting_) {
    return;  // 已进入 P2P/已连接阶段的断开由 session 回调处理。
  }
  rdDebug("direct-viewer", "signaling failed: %s", reason.toUtf8().constData());
  session_->stop();
  // 连接建立过一次的会话异常断开（含重试期间被拒：helper 忙/会话切换中）
  // 走自动重连；首次手动连接失败仍直接报错。
  scheduleAutoReconnect(tr("连接失败: %1").arg(reason));
}

void DirectViewerWindow::sendInputEvent(const rd::InputEvent& ev) {
  session_->sendInput(ev);
}

void DirectViewerWindow::resetForRetry(const QString& statusText) {
  connecting_ = false;
  pendingOffer_.clear();
  offerSent_ = false;
  connectBtn_->setText(tr("连接"));
  connectBtn_->setEnabled(true);
  showVideoBtn_->hide();
  setStatus(statusText);
}

void DirectViewerWindow::onSessionConnected() {
  rdDebug("direct-viewer", "session connected");
  everConnected_ = true;
  autoAttempts_ = 0;
  setStatus(tr("已连接 — 远端画面已弹出"));
  // 连接建立即弹出独立远端画面窗口（工具栏可退出全屏）。
  videoWindow_->showFullscreen();
  showVideoBtn_->show();
}

void DirectViewerWindow::onSessionDisconnected() {
  rdDebug("direct-viewer", "session disconnected");
  signaling_->abort();
  videoWindow_->close();
  if (userStopped_) {
    userStopped_ = false;  // 消费一次主动断开意图
    resetForRetry(tr("已断开连接"));
    return;
  }
  scheduleAutoReconnect(tr("连接断开"));
}

void DirectViewerWindow::onSessionFailed(const QString& reason) {
  // 自动重连期间只改状态栏，不弹模态框（弹窗会卡住后续重试）。
  if (!userStopped_ && everConnected_ && reconnectTimer_ &&
      (reconnectTimer_->isActive() || autoAttempts_ > 0)) {
    setStatus(tr("错误: %1").arg(reason));
    rdDebug("direct-viewer", "suppressed popup during auto-retry: %s",
            reason.toUtf8().constData());
    return;
  }
  SessionWindow::onSessionFailed(reason);
}

void DirectViewerWindow::closeEvent(QCloseEvent* event) {
  if (session_->role() != RemoteDesktopSession::Role::None || connecting_) {
    signaling_->sendBye();
    session_->stop();
    signaling_->abort();
  }
  userStopped_ = true;
  reconnectTimer_->stop();
  event->accept();
}

}  // namespace rd