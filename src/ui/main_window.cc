#include "ui/main_window.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>

#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include <atomic>

#include <QComboBox>
#include <QTimer>

#include <screen_capture/capture.hpp>

#include "core/input_desktop.h"
#include "debug_log.h"
#include "ui/remote_video_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>  // ShellExecuteW（提权重启；LEAN_AND_MEAN 不含 shellapi）

namespace rd {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  session_ = new RemoteDesktopSession(this);
  connect(session_, &RemoteDesktopSession::failed, this,
          [this](const QString& r) {
            setStatus(tr("错误: %1").arg(r));
            QMessageBox::warning(this, tr("远程桌面"), r);
          });
  connect(session_, &RemoteDesktopSession::connected, this, [this] {
    setStatus(tr("已连接 — 双击预览画面可全屏"));
    // Same-machine testing: auto-popping fullscreen would mirror the captured
    // screen into itself; the embedded preview shows the stream instead.
  });
  connect(session_, &RemoteDesktopSession::disconnected, this, [this] {
    setStatus(tr("连接断开"));
    videoWindow_->close();
  });
  // Frames arrive on libdatachannel threads; queue them to the video window.
  videoWindow_ = new RemoteVideoWindow(this);
  connect(session_, &RemoteDesktopSession::frameReady, videoWindow_,
          &RemoteVideoWindow::setFrame, Qt::QueuedConnection);  connect(videoWindow_, &RemoteVideoWindow::disconnectRequested, this, [this] {
    if (session_->role() != RemoteDesktopSession::Role::None) {
      session_->stop();
      setStatus(tr("已断开连接"));
    }
  });
  // Visible confirmation that a forwarded input event was injected on the
  // host (throttled; mouse moves fire at up to ~60 Hz).
  connect(session_, &RemoteDesktopSession::inputInjected, this, [this] {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastInjectStatusMs_ >= 500) {
      lastInjectStatusMs_ = now;
      statusBar()->showMessage(tr("输入注入中 — 鼠标/键盘已转发到被控端"), 1500);
    }
  });
  // Forward mouse/keyboard on the remote video to the host.
  connect(videoWindow_, &RemoteVideoWindow::inputEvent, this,
          &MainWindow::sendInputEvent);

  // 本机锁屏/UAC 状态（采集线程触发；queued 到 UI 线程）。
  connect(session_, &RemoteDesktopSession::hostSecureDesktopChanged, this,
          &MainWindow::applyHostLockHint);
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

  auto* tabs = new QTabWidget(this);
  tabs->addTab(makeHostTab(), tr("被控端 (Host)"));
  tabs->addTab(makeViewerTab(), tr("主控端 (Viewer)"));
  setCentralWidget(tabs);
  // The embedded preview is built inside makeViewerTab(); feed it frames now.
  connect(session_, &RemoteDesktopSession::frameReady, viewerPreview_,
          &VideoWidget::setFrame, Qt::QueuedConnection);
  setWindowTitle(tr("远程桌面 — 手动 SDP 交换"));
  resize(1100, 720);
  setStatus(tr("就绪"));

  // Poll pipeline counters once a second: they localise a "connected but no
  // picture" report to the stage that stopped (capture/encode/send vs
  // receive/decode/display).
  diagTimer_ = new QTimer(this);
  connect(diagTimer_, &QTimer::timeout, this, &MainWindow::updateDiag);
  diagTimer_->start(1000);
}

void MainWindow::updateDiag() {
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
  if (hostState_ && role == RemoteDesktopSession::Role::Host) {
    hostState_->setText(text);
  }
  if (viewerState_ && role == RemoteDesktopSession::Role::Viewer) {
    viewerState_->setText(text);
  }
  videoWindow_->setDiagText(text);
  if (viewerPreview_) {
    viewerPreview_->setDiagText(text);
  }
}

void MainWindow::sendInputEvent(const rd::InputEvent& ev) {
  static std::atomic<int> dbg{0};
  if (dbg.fetch_add(1) < 10) {
    rdDebug("mainwin", "inputEvent type=%d -> sendInput", (int)ev.type);
  }
  session_->sendInput(ev);
}

QWidget* MainWindow::makeHostTab() {
  auto* root = new QWidget(this);
  auto* lay = new QVBoxLayout(root);

  hostState_ = new QLabel(
      tr("1) 选择屏幕并生成 Answer，2) 把 Answer 发给主控端"), root);
  lay->addWidget(hostState_);

  auto* screenRow = new QHBoxLayout();
  screenRow->addWidget(new QLabel(tr("共享的屏幕:"), root));
  hostScreenBox_ = new QComboBox(root);
  for (const auto& s : scl::EnumSources(SCL_SOURCE_SCREEN)) {
    hostScreenBox_->addItem(QString::fromStdString(s.title),
                            QVariant::fromValue<qint64>(s.id));
  }
  if (hostScreenBox_->count() == 0) {
    hostScreenBox_->addItem(tr("屏幕 0"), QVariant::fromValue<qint64>(0));
  }
  hostScreenBox_->setToolTip(
      tr("同机测试时请选择主控端全屏窗口所在的显示器之外的屏幕，\n"
         "否则画面是递归镜像（全黑+提示文字）。"));
  screenRow->addWidget(hostScreenBox_, 1);
  lay->addLayout(screenRow);

  lay->addWidget(new QLabel(tr("粘贴主控端的 Offer:"), root));
  hostOffer_ = new QPlainTextEdit(root);
  hostOffer_->setPlaceholderText(tr("在此粘贴 Offer SDP..."));
  lay->addWidget(hostOffer_, 2);

  auto* btnRow = new QHBoxLayout();
  hostAnswerBtn_ = new QPushButton(tr("生成 Answer"), root);
  hostStartBtn_ = new QPushButton(tr("开始推流"), root);
  hostStartBtn_->setEnabled(false);
  btnRow->addWidget(hostAnswerBtn_);
  btnRow->addWidget(hostStartBtn_);
  lay->addLayout(btnRow);

  // 锁屏/UAC（安全桌面）提示与提权入口：未提权进程打不开安全桌面，
  // 锁屏后画面与控制中断；提权后 GDI 可捕获锁屏、SendInput 可注入。
  hostLockHint_ = new QLabel(root);
  hostLockHint_->setWordWrap(true);
  hostLockHint_->hide();
  lay->addWidget(hostLockHint_);
  hostElevateBtn_ = new QPushButton(tr("以管理员身份重启"), root);
  hostElevateBtn_->setToolTip(
      tr("锁屏/UAC 时的采集与控制需要管理员权限；重启后需重新交换 SDP。"));
  connect(hostElevateBtn_, &QPushButton::clicked, this,
          &MainWindow::onHostElevateRestart);
  lay->addWidget(hostElevateBtn_);

  lay->addWidget(new QLabel(tr("你的 Answer（复制发给主控端）:"), root));
  hostAnswer_ = new QPlainTextEdit(root);
  hostAnswer_->setReadOnly(true);
  lay->addWidget(hostAnswer_, 3);

  connect(hostAnswerBtn_, &QPushButton::clicked, this,
          &MainWindow::onHostGenerateAnswer);
  connect(hostStartBtn_, &QPushButton::clicked, this,
          &MainWindow::onHostToggleStream);
  return root;
}

QWidget* MainWindow::makeViewerTab() {
  // Embedded live preview: visible on the Viewer tab itself so same-machine
  // testing does not depend on the fullscreen popup (which would otherwise
  // mirror the captured screen into itself). Double-click pops the fullscreen
  // remote window back up.
  viewerPreview_ = new VideoWidget;
  viewerPreview_->setToolTip(tr("双击打开/关闭全屏远端画面窗口"));
  connect(viewerPreview_, &VideoWidget::inputEvent, this,
          &MainWindow::sendInputEvent);

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

  viewerState_ = new QLabel(tr("1) 创建 Offer 发给被控端，2) 粘贴 Answer 并连接"), panel);
  lay->addWidget(viewerState_);

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
          &MainWindow::onViewerCreateOffer);
  connect(viewerConnectBtn_, &QPushButton::clicked, this,
          &MainWindow::onViewerConnect);


  return panel;
}

void MainWindow::setStatus(const QString& text) {
  statusBar()->showMessage(text);
  if (hostState_) {
    hostState_->setText(text);
  }
  if (viewerState_) {
    viewerState_->setText(text);
  }
}

void MainWindow::onHostGenerateAnswer() {
  const QString offer = hostOffer_->toPlainText().trimmed();
  if (offer.isEmpty()) {
    QMessageBox::information(this, tr("远程桌面"), tr("请先粘贴主控端的 Offer"));
    return;
  }
  hostScreenId_ = hostScreenBox_->currentData().toLongLong();
  const QString answer = session_->hostAcceptOfferChecked(offer, hostScreenId_);
  if (answer.isEmpty()) {
    // startHost/hostAcceptOffer failed — reset so the user can retry.
    session_->stop();
    QMessageBox::warning(this, tr("远程桌面"),
                         tr("生成 Answer 失败，请检查 Offer 或屏幕捕获"));
    return;
  }
  hostAnswer_->setPlainText(answer);
  hostStartBtn_->setEnabled(true);
  applyHostLockHint(false, true);  // 启动即预检：未提权时给出常驻提醒
  setStatus(tr("Answer 已生成 — 复制给主控端，连接建立后将自动推流"));
}

void MainWindow::onHostToggleStream() {
  // Capture loop already runs; this reserves space for pause/resume.
  setStatus(tr("推流中 — 等待主控端连接"));
}

void MainWindow::onViewerCreateOffer() {
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

void MainWindow::onViewerConnect() {
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
  viewerPreview_->show();
}

void MainWindow::applyHostLockHint(bool secure, bool capturable) {
  if (!hostLockHint_) {
    return;
  }
  if (secure) {
    if (capturable) {
      hostLockHint_->setText(
          tr("已锁屏 — 正在捕获安全桌面（锁屏画面与输入正常）"));
      hostLockHint_->setStyleSheet("color: #2e7d32;");
    } else {
      hostLockHint_->setText(
          tr("已锁屏 — 本进程打不开安全桌面（需管理员运行），画面与控制"
             "已中断；解锁后自动恢复"));
      hostLockHint_->setStyleSheet("color: #c62828;");
    }
    hostLockHint_->show();
    return;
  }
  // 未锁屏：仅未提权时保留常驻提醒（提权下隐藏）。
  hostLockHint_->setStyleSheet("color: #8a6d3b;");
  if (rd::CanOpenSecureDesktop()) {
    hostLockHint_->hide();
  } else {
    hostLockHint_->setText(
        tr("未以管理员运行：锁屏/UAC 期间无法查看与控制；需要时点"
           "\"以管理员身份重启\""));
    hostLockHint_->show();
  }
}

void MainWindow::onHostElevateRestart() {
  wchar_t exePath[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  const HINSTANCE r = ShellExecuteW(nullptr, L"runas", exePath, nullptr,
                                    nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(r) > 32) {
    // 新实例已提权启动，退出当前实例；两侧需重新交换 SDP。
    session_->stop();
    close();
  } else {
    setStatus(tr("未以管理员身份重启（已取消或失败）"));
  }
}

}  // namespace rd
