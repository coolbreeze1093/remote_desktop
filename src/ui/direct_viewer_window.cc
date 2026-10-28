#include "ui/direct_viewer_window.h"

#include <QBuffer>
#include <QCloseEvent>
#include <QCursor>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
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
// 历史连接记录条数（主界面卡片展示，QSettings 持久化）。
constexpr int kMaxHistory = 5;
// 历史连接卡片尺寸（缩略图 16:9 + 下方地址文字）。
constexpr int kThumbW = 192;
constexpr int kThumbH = 108;
constexpr int kCardW = 208;
constexpr int kCardH = 150;

// 拆 "host[:port]"：冒号后全为数字才视作端口（兼容在 IP 框直接粘 host:port）。
bool splitAddress(const QString& text, QString* host, quint16* port) {
  const int colon = text.lastIndexOf(':');
  if (colon > 0) {
    bool ok = false;
    const quint16 p = text.mid(colon + 1).toUShort(&ok);
    if (ok && p > 0) {
      *host = text.left(colon).trimmed();
      *port = p;
      return !host->isEmpty();
    }
  }
  *host = text;
  return !text.isEmpty();
}

// 把 QImage 居中裁剪缩放成卡片缩略图尺寸。
QImage fitThumbnail(const QImage& image) {
  const QImage scaled =
      image.scaled(kThumbW, kThumbH, Qt::KeepAspectRatioByExpanding,
                   Qt::SmoothTransformation);
  return scaled.copy((scaled.width() - kThumbW) / 2,
                     (scaled.height() - kThumbH) / 2, kThumbW, kThumbH);
}

// 卡片图标：优先用该地址上次连接时抓到的画面，没有则画深色显示器占位图；
// 统一圆角裁剪，仿地址簿卡片风格。
QPixmap makeCardIcon(const QString& entry) {
  QImage thumb(kThumbW, kThumbH, QImage::Format_ARGB32_Premultiplied);
  const QByteArray b64 =
      QSettings(QStringLiteral("remote_desktop"), QStringLiteral("ui"))
          .value(QStringLiteral("directViewer/thumbs/") + entry)
          .toByteArray();
  QImage stored;
  if (!b64.isEmpty()) {
    stored.loadFromData(QByteArray::fromBase64(b64), "JPEG");
  }
  if (!stored.isNull()) {
    thumb = fitThumbnail(stored);
  } else {
    thumb.fill(QColor(0x1a, 0x24, 0x3a));
    QPainter p(&thumb);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient grad(0, 0, 0, kThumbH);
    grad.setColorAt(0, QColor(0x2b, 0x3a, 0x55));
    grad.setColorAt(1, QColor(0x1a, 0x24, 0x3a));
    p.fillRect(thumb.rect(), grad);
    // 显示器轮廓 + 支架。
    p.setPen(QPen(QColor(0x9c, 0xbe, 0xe6), 3));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(48, 24, 96, 56, 6, 6);
    p.drawLine(76, 80, 116, 80);
    p.drawLine(96, 80, 96, 90);
    p.drawLine(70, 92, 122, 92);
    p.end();
  }
  QPixmap result(kThumbW, kThumbH);
  result.fill(Qt::transparent);
  QPainter pp(&result);
  pp.setRenderHint(QPainter::Antialiasing);
  QPainterPath path;
  path.addRoundedRect(0, 0, kThumbW, kThumbH, 8, 8);
  pp.setClipPath(path);
  pp.drawPixmap(0, 0, QPixmap::fromImage(thumb));
  pp.end();
  return result;
}
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
  // 弹窗工具栏的画质档位 → 会话（控制通道下发到被控端编码器）。
  connect(videoWindow_, &RemoteVideoWindow::qualityModeSelected, this,
          [this](int mode) { session_->setVideoQualityMode(mode); });
  // 弹窗内切换被控端屏幕：屏幕 id 只能随 Offer 生效，会话中切换即换屏
  // 重连（信令断开→服务端重拉 helper→新 Offer 带上所选屏幕）。
  connect(videoWindow_, &RemoteVideoWindow::screenSelected, this,
          [this](qint64 screenId) {
            if (session_->role() == RemoteDesktopSession::Role::None ||
                connecting_ || autoPort_ == 0) {
              return;  // 未连接时选择只影响下一次 Offer
            }
            rdDebug("direct-viewer", "switch screen -> %lld, reconnecting",
                    (long long)screenId);
            userStopped_ = false;
            autoAttempts_ = 0;  // 换屏重连给满重试次数
            reconnectTimer_->stop();
            signaling_->sendBye();
            session_->stop();
            signaling_->abort();
            startConnection(autoHost_, autoPort_);
          });
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

  // 连接行：IP + 端口 + 连接按钮（历史连接在下方空白区以卡片展示）。
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

  // 历史连接：地址簿风格卡片（缩略图 + 地址），点击直连，右键删除单条。
  historyBox_ = new QWidget(panel);
  historyBox_->setStyleSheet(QStringLiteral(
      "QToolButton { background: white; border: 1px solid #dcdcdc;"
      "  border-radius: 8px; font-size: 9pt; }"
      "QToolButton:hover { border: 1px solid #4a90d9; background: #f5f9ff; }"
      "QToolButton:pressed { background: #e8f1fc; }"));
  historyLay_ = new QVBoxLayout(historyBox_);
  historyLay_->setContentsMargins(0, 16, 0, 0);
  historyLay_->setSpacing(8);
  historyCaption_ = new QLabel(tr("历史连接"), historyBox_);
  historyCaption_->setStyleSheet(
      QStringLiteral("color: #666666; font-weight: bold;"));
  historyLay_->addWidget(historyCaption_);
  historyCards_ = new QHBoxLayout();
  historyCards_->setSpacing(12);
  historyLay_->addLayout(historyCards_);
  lay->addWidget(historyBox_);
  lay->addStretch(1);
  loadAddressHistory();  // 启动时已有历史则立即填充卡片

  // 收到解码帧时抓首次画面做该地址的缩略图（与画面窗口同一信号源）。
  connect(session_, &RemoteDesktopSession::frameReady, this,
          &DirectViewerWindow::captureThumbnail, Qt::QueuedConnection);

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
  QString host;
  quint16 port = 0;
  if (!splitAddress(hostEdit_->text().trimmed(), &host, &port)) {
    QMessageBox::information(this, tr("远程桌面"),
                             tr("请填写被控端 IP 与有效端口"));
    return;
  }
  if (port == 0) {
    bool ok = false;
    port = portEdit_->text().toUShort(&ok);
    if (!ok || port == 0) {
      QMessageBox::information(this, tr("远程桌面"),
                               tr("请填写被控端 IP 与有效端口"));
      return;
    }
  }
  startConnection(host, port);
}

void DirectViewerWindow::loadAddressHistory() {
  history_ = QSettings(QStringLiteral("remote_desktop"), QStringLiteral("ui"))
                 .value(QStringLiteral("directViewer/recentAddresses"))
                 .toStringList();
  while (history_.size() > kMaxHistory) {
    history_.removeLast();
  }
  rebuildHistoryPanel();
}

void DirectViewerWindow::rememberAddress(const QString& host, quint16 port) {
  const QString entry = QStringLiteral("%1:%2").arg(host).arg(port);
  history_.removeAll(entry);
  history_.prepend(entry);
  while (history_.size() > kMaxHistory) {
    history_.removeLast();
  }
  QSettings(QStringLiteral("remote_desktop"), QStringLiteral("ui"))
      .setValue(QStringLiteral("directViewer/recentAddresses"), history_);
  rebuildHistoryPanel();
}

void DirectViewerWindow::rebuildHistoryPanel() {
  // 清空旧卡片后按 history_ 重建（缩略图 + 地址，点击直连，右键删除）。
  while (QLayoutItem* item = historyCards_->takeAt(0)) {
    if (QWidget* w = item->widget()) {
      w->deleteLater();
    }
    delete item;
  }
  if (history_.isEmpty()) {
    historyBox_->hide();
    return;
  }
  for (const QString& entry : history_) {
    auto* card = new QToolButton(historyBox_);
    card->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    card->setIcon(makeCardIcon(entry));
    card->setIconSize(QSize(kThumbW, kThumbH));
    card->setText(entry);
    card->setFixedSize(kCardW, kCardH);
    connect(card, &QToolButton::clicked, this, [this, entry] {
      QString host;
      quint16 port = 0;
      if (!splitAddress(entry, &host, &port) || port == 0) {
        return;
      }
      hostEdit_->setText(host);
      portEdit_->setText(QString::number(port));
      startConnection(host, port);  // 已连接时由 startConnection 先拆旧会话
    });
    card->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(card, &QToolButton::customContextMenuRequested, this,
            [this, card, entry] {
              QMenu menu(card);
              QAction* del = menu.addAction(tr("删除这条记录"));
              if (menu.exec(QCursor::pos()) != del) {
                return;
              }
              history_.removeOne(entry);
              QSettings(QStringLiteral("remote_desktop"),
                        QStringLiteral("ui"))
                  .setValue(QStringLiteral("directViewer/recentAddresses"),
                            history_);
              QSettings(QStringLiteral("remote_desktop"),
                        QStringLiteral("ui"))
                  .remove(QStringLiteral("directViewer/thumbs/") + entry);
              rebuildHistoryPanel();
            });
    historyCards_->addWidget(card);
  }
  historyCards_->addStretch(1);
  historyBox_->show();
}

// 连接建立后收到首批解码帧时，抓第一帧做该地址的卡片缩略图（JPEG 存储）。
void DirectViewerWindow::captureThumbnail(const QImage& image) {
  if (thumbCaptured_ || autoPort_ == 0 || image.isNull()) {
    return;
  }
  thumbCaptured_ = true;
  const QImage thumb = fitThumbnail(image);
  QBuffer buf;
  if (!thumb.save(&buf, "JPEG", 85)) {
    return;
  }
  const QString entry =
      QStringLiteral("%1:%2").arg(autoHost_).arg(autoPort_);
  QSettings(QStringLiteral("remote_desktop"), QStringLiteral("ui"))
      .setValue(QStringLiteral("directViewer/thumbs/") + entry,
                buf.data().toBase64());
  if (history_.contains(entry)) {
    rebuildHistoryPanel();  // 卡片已在列表中：刷新图标
  }
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
  thumbCaptured_ = false;  // 新会话重抓缩略图
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
  // 列表填入远端画面弹窗的工具栏下拉框（保留当前选择；重连沿用）。
  videoWindow_->setScreens(screenIds, screenTitles);
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
  if (screensKnown_ && videoWindow_->currentScreenId() != 0) {
    const qint64 id = videoWindow_->currentScreenId();
    rdDebug("direct-viewer",
            "signaling connected, sending offer len=%d screen=%lld",
            pendingOffer_.size(), (long long)id);
    setStatus(tr("已连上服务端，交换信令中（屏幕：%1）...")
                  .arg(videoWindow_->screenTitle()));
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
  rememberAddress(autoHost_, autoPort_);  // 连接成功才计入历史
  setStatus(tr("已连接 — 远端画面已弹出"));
  // 连接建立即弹出独立远端画面窗口（工具栏可退出全屏）。
  videoWindow_->showFullscreen();
  showVideoBtn_->show();
  // 引擎随会话重建，档位回到默认：重放弹窗里用户选的档位。
  session_->setVideoQualityMode(videoWindow_->currentQualityMode());
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