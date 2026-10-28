#include "ui/direct_host_window.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <string>
#include <vector>

#include <screen_capture/capture.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>  // ShellExecuteW（装/卸服务走 UAC；LEAN_AND_MEAN 不含）

#include "debug_log.h"
#include "pipe_proto.h"
#include "service/host_service_client.h"
#include "session/direct_signaling.h"

namespace rd {

namespace {
constexpr quint16 kDefaultPort = 48010;
}  // namespace

DirectHostWindow::DirectHostWindow(QWidget* parent)
    : SessionWindow(parent) {
  setWindowTitle(tr("远程桌面 直连被控端 — 监听端口等主控端来连"));
  resize(560, 430);
  setCentralWidget(makeHostPanel());

  signaling_ = new DirectSignalingServer(this);
  connect(signaling_, &DirectSignalingServer::clientArrived, this,
          [this](const QString& peer) {
            rdDebug("direct-host", "client arrived %s", peer.toUtf8().constData());
            setStatus(tr("主控端已连线（%1），等待 Offer...").arg(peer));
          });
  connect(signaling_, &DirectSignalingServer::offerReceived, this,
          &DirectHostWindow::onOfferReceived);
  connect(signaling_, &DirectSignalingServer::clientLeft, this,
          &DirectHostWindow::onClientLeft);
  connect(signaling_, &DirectSignalingServer::listenFailed, this,
          [this](const QString& reason) {
            listening_ = false;
            listenBtn_->setText(tr("开始监听"));
            setStatus(tr("监听失败: %1").arg(reason));
          });

  // 本机模式的锁屏/UAC 提示（推流与控制会中断，解锁后自动恢复）；服务模式
  // 的同类提示走 onSvcEvent 的 secure 事件，共用这一个标签。
  connect(session_, &RemoteDesktopSession::hostSecureDesktopChanged, this,
          [this](bool secure, bool capturable) {
            if (!secure) {
              lockHint_->hide();
              return;
            }
            lockHint_->setText(capturable
                                   ? tr("已锁屏 — 正在捕获锁屏画面，输入已切到安全桌面")
                                   : tr("已锁屏 — 本进程未提权，无法捕获/控制锁屏；"
                                        "解锁后自动恢复"));
            lockHint_->setStyleSheet(capturable ? "color: #2e7d32;"
                                                : "color: #c62828;");
            lockHint_->show();
          });

  // 被控服务面板：后台线程连 rd_host_service helper 的命名管道，事件经
  // 队列信号回到 UI 线程。服务未装/未起时回退本机模式，流程照常可用。
  svc_ = new HostServiceClient(this);
  connect(svc_, &HostServiceClient::serviceInfo, this,
          &DirectHostWindow::onSvcInfo, Qt::QueuedConnection);
  connect(svc_, &HostServiceClient::eventReceived, this,
          &DirectHostWindow::onSvcEvent, Qt::QueuedConnection);
  connect(svc_, &HostServiceClient::serviceGone, this,
          &DirectHostWindow::onSvcGone, Qt::QueuedConnection);
  connect(svc_, &HostServiceClient::serviceStatus, this,
          &DirectHostWindow::onSvcStatus, Qt::QueuedConnection);
  svc_->start();

  setStatus(tr("就绪 — 要锁屏/UAC 可控请先「安装被控服务」；"
               "选好屏幕与端口后点\"开始监听\""));
}

QWidget* DirectHostWindow::makeHostPanel() {
  auto* panel = new QWidget();
  auto* lay = new QVBoxLayout(panel);

  // ---- 被控服务面板：SYSTEM helper 承担采集/编码/注入（锁屏/UAC/登录可控）。
  svcState_ = new QLabel(tr("被控服务: 检测中…"), panel);
  svcState_->setWordWrap(true);
  lay->addWidget(svcState_);

  auto* svcRow = new QHBoxLayout();
  svcInstallBtn_ = new QPushButton(tr("安装被控服务"), panel);
  svcInstallBtn_->setToolTip(
      tr("把 rd_host_service 注册为 Windows 服务（SYSTEM，开机自启）。\n"
         "需要管理员确认；安装后锁屏/UAC/登录界面均可查看与控制。"));
  svcUninstallBtn_ = new QPushButton(tr("卸载被控服务"), panel);
  svcUninstallBtn_->setEnabled(false);
  svcRow->addWidget(svcInstallBtn_);
  svcRow->addWidget(svcUninstallBtn_);
  svcRow->addStretch(1);
  lay->addLayout(svcRow);

  auto* screenRow = new QHBoxLayout();
  screenRow->addWidget(new QLabel(tr("共享的屏幕:"), panel));
  screenBox_ = new QComboBox(panel);
  fillLocalScreens();
  // 服务模式：切换屏幕即推给 helper（startHost 缺省屏）。
  connect(screenBox_, qOverload<int>(&QComboBox::activated), this,
          [this](int) { pushDefaultScreen(); });
  screenRow->addWidget(screenBox_, 1);
  lay->addLayout(screenRow);

  auto* portRow = new QHBoxLayout();
  portRow->addWidget(new QLabel(tr("监听端口:"), panel));
  portEdit_ = new QLineEdit(QString::number(kDefaultPort), panel);
  portRow->addWidget(portEdit_, 1);
  lay->addLayout(portRow);

  lay->addWidget(new QLabel(
      tr("主控端在本程序输入\"本机IP:端口\"即可直连；\n"
         "跨网段需要把该端口转发/放行到本机。"), panel));

  listenBtn_ = new QPushButton(tr("开始监听"), panel);
  connect(listenBtn_, &QPushButton::clicked, this,
          &DirectHostWindow::onToggleListen);
  lay->addWidget(listenBtn_);

  stateLabel_ = new QLabel(panel);
  stateLabel_->setWordWrap(true);
  lay->addWidget(stateLabel_);

  svcStatus_ = new QLabel(panel);
  svcStatus_->setWordWrap(true);
  svcStatus_->hide();
  lay->addWidget(svcStatus_);

  lockHint_ = new QLabel(panel);
  lockHint_->setWordWrap(true);
  lockHint_->hide();
  lay->addWidget(lockHint_);

  lay->addStretch(1);

  connect(svcInstallBtn_, &QPushButton::clicked, this,
          &DirectHostWindow::onSvcInstall);
  connect(svcUninstallBtn_, &QPushButton::clicked, this,
          &DirectHostWindow::onSvcUninstall);
  return panel;
}

void DirectHostWindow::fillLocalScreens() {
  screenBox_->clear();
  for (const auto& s : scl::EnumSources(SCL_SOURCE_SCREEN)) {
    screenBox_->addItem(QString::fromStdString(s.title),
                        QVariant::fromValue<qint64>(s.id));
  }
  if (screenBox_->count() == 0) {
    screenBox_->addItem(tr("屏幕 0"), QVariant::fromValue<qint64>(0));
  }
}

void DirectHostWindow::autoListen(quint16 port) {
  portEdit_->setText(QString::number(port));
  startListening();
}

void DirectHostWindow::onToggleListen() {
  if (listening_) {
    stopListening();
    // 仅本机模式会走到这里（服务模式信令由服务承载，面板不监听）。
    session_->stop();
    setStatus(tr("已停止监听"));
    return;
  }
  if (svcConnected_) {
    // 服务模式：TCP 信令由 rd_host_service 承载，面板无需也不应再监听。
    setStatus(tr("服务模式：信令由被控服务承载（端口 48010），无需监听 —"
                 "主控端直接连本机 IP 即可"));
    return;
  }
  startListening();
}

void DirectHostWindow::startListening() {
  if (svcConnected_) {
    setStatus(tr("服务模式：信令由被控服务承载（端口 48010），无需监听"));
    return;
  }
  bool ok = false;
  const quint16 port = portEdit_->text().toUShort(&ok);
  if (!ok || port == 0) {
    QMessageBox::warning(this, tr("远程桌面"), tr("端口无效"));
    return;
  }
  listening_ = signaling_->listen(port);
  if (listening_) {
    listenBtn_->setText(tr("停止监听"));
    rdDebug("direct-host", "listening port=%u mode=%s", (unsigned)port,
            svcConnected_ ? "service" : "local");
    setStatus(tr("正在监听 0.0.0.0:%1（%2）...")
                  .arg(port)
                  .arg(svcConnected_ ? tr("服务模式，推流走被控服务")
                                     : tr("本机模式")));
  }
}

void DirectHostWindow::stopListening() {
  signaling_->stopListen();
  listening_ = false;
  listenBtn_->setText(tr("开始监听"));
  rdDebug("direct-host", "listen stopped");
}

void DirectHostWindow::onOfferReceived(const QString& offer) {
  // 只可能来自本机模式的监听（服务模式下信令在服务本体，不经面板）。
  rdDebug("direct-host", "offer received len=%d", offer.size());
  setStatus(tr("收到 Offer，正在生成 Answer 并启动推流（ICE 收集最长 ~10s）..."));
  const int64_t screenId = screenBox_->currentData().toLongLong();
  const QString answer = session_->hostAcceptOfferChecked(offer, screenId);
  if (answer.isEmpty()) {
    rdDebug("direct-host", "accept offer failed");
    setStatus(tr("生成 Answer 失败（offer 无效或采集初始化失败），继续监听"));
    return;
  }
  rdDebug("direct-host", "answer sent len=%d", answer.size());
  signaling_->sendAnswer(answer);
  setStatus(tr("Answer 已回，等待 P2P 建立..."));
}

void DirectHostWindow::onClientLeft() {
  if (session_->role() == RemoteDesktopSession::Role::Host) {
    // 本机模式：客户端通过信令主动断开（BYE）：停止本机推流。
    session_->stop();
  }
  if (listening_) {
    setStatus(tr("信令连接已关闭，继续监听..."));
  }
}

void DirectHostWindow::onSessionConnected() {
  rdDebug("direct-host", "session connected");
  setStatus(tr("已连接 — 主控端正在查看与操作本机"));
}

void DirectHostWindow::onSessionDisconnected() {
  rdDebug("direct-host", "session disconnected");
  session_->stop();
  if (listening_) {
    setStatus(tr("会话结束，继续监听..."));
  }
}

// ---------- 被控服务面板 ----------

void DirectHostWindow::onSvcInfo(const QString& json) {
  const std::string j = json.toStdString();
  const std::string version = rdpipe::JsonGetString(j, "version");
  const long elevated = rdpipe::JsonGetInt(j, "elevated");
  std::vector<long long> ids;
  std::vector<std::string> titles;
  rdpipe::JsonGetArrayInt(j, "screenIds", &ids);
  rdpipe::JsonGetArrayString(j, "screenTitles", &titles);

  svcConnected_ = true;
  // 屏幕列表换成服务枚举的结果：服务模式下采集在服务进程，以它的屏幕为准。
  screenBox_->clear();
  for (size_t i = 0; i < titles.size(); ++i) {
    const long long id = i < ids.size() ? ids[i] : (long long)i;
    screenBox_->addItem(QString::fromStdString(titles[i]),
                        QVariant::fromValue<qint64>(id));
  }
  if (screenBox_->count() == 0) {
    screenBox_->addItem(tr("屏幕 0"), QVariant::fromValue<qint64>(0));
  }
  pushDefaultScreen();  // helper 重启会丢默认屏：每次连上即补推
  if (listening_) {
    // 服务就绪：面板监听让位给服务信令（端口 48010 归 rd_host_service）。
    stopListening();
  }
  svcState_->setText(
      tr("被控服务: 已连接 helper（%1%2）")
          .arg(QString::fromStdString(version))
          .arg(elevated ? tr("，SYSTEM 身份") : tr("，非 SYSTEM（异常）")));
  svcState_->setStyleSheet(elevated ? "color: #2e7d32;" : "color: #c62828;");
  svcInstallBtn_->setEnabled(false);
  svcUninstallBtn_->setEnabled(true);
  if (svcConnected_) {
    setStatus(tr("服务模式：信令由被控服务承载（端口 48010），主控端直连即可"));
  }
  rdDebug("direct-host", "service helper connected (%s)", version.c_str());
}

void DirectHostWindow::onSvcEvent(const QString& json) {
  const std::string j = json.toStdString();
  const std::string type = rdpipe::JsonGetString(j, "type");
  if (type == "state") {
    const std::string state = rdpipe::JsonGetString(j, "state");
    svcStateText_ =
        tr("会话状态: %1").arg(QString::fromStdString(state));
    const std::string detail = rdpipe::JsonGetString(j, "detail");
    if (!detail.empty()) {
      svcStateText_ += tr("（%1）").arg(QString::fromStdString(detail));
    }
    if (state == "connected") {
      setStatus(tr("已连接 — 主控端正在查看与操作本机（服务推流）"));
    } else if (state == "closed" || state == "failed") {
      if (listening_) {
        setStatus(tr("会话结束，继续监听..."));
      }
    }
    refreshSvcStatus();
  } else if (type == "secure") {
    const bool secure = rdpipe::JsonGetInt(j, "secure") != 0;
    const bool capturable = rdpipe::JsonGetInt(j, "capturable") != 0;
    if (!secure) {
      lockHint_->hide();
    } else if (capturable) {
      lockHint_->setText(
          tr("已锁屏/UAC — 服务（SYSTEM）正在捕获安全桌面，画面与输入正常"));
      lockHint_->setStyleSheet("color: #2e7d32;");
      lockHint_->show();
    } else {
      lockHint_->setText(
          tr("已锁屏/UAC — 服务未能打开安全桌面（异常），请回报 rd_debug.log"));
      lockHint_->setStyleSheet("color: #c62828;");
      lockHint_->show();
    }
  } else if (type == "error") {
    // 信令中继在服务本体：这里只展示，不再负责断开等 Answer 的客户端。
    const QString msg =
        QString::fromStdString(rdpipe::JsonGetString(j, "msg"));
    svcStateText_ = tr("错误: %1").arg(msg);
    refreshSvcStatus();
  } else if (type == "stats") {
    static const char* kLevelNames[] = {"流畅", "高清", "超清"};
    svcStatsText_ =
        tr("管道: 捕获 %1 / 编码 %2 / 已发送 %3 / 丢弃 %4")
            .arg(rdpipe::JsonGetInt(j, "captured"))
            .arg(rdpipe::JsonGetInt(j, "encoded"))
            .arg(rdpipe::JsonGetInt(j, "sent"))
            .arg(rdpipe::JsonGetInt(j, "dropped"));
    const long level = rdpipe::JsonGetInt(j, "qualityLevel", -1);
    if (level >= 0 && level <= 2) {
      svcStatsText_ += tr(" | 档位: %1 %2×%3 编码%4ms")
                           .arg(QString::fromUtf8(kLevelNames[level]))
                           .arg(rdpipe::JsonGetInt(j, "encodeWidth"))
                           .arg(rdpipe::JsonGetInt(j, "encodeHeight"))
                           .arg(rdpipe::JsonGetInt(j, "encodeMs"));
    }
    refreshSvcStatus();
  }
}

void DirectHostWindow::refreshSvcStatus() {
  QString text = svcStateText_;
  if (!svcStatsText_.isEmpty()) {
    if (!text.isEmpty()) {
      text += "\n";
    }
    text += svcStatsText_;
  }
  if (text.isEmpty()) {
    svcStatus_->hide();
    return;
  }
  svcStatus_->setText(text);
  svcStatus_->show();
}

// 未连接 helper 时的状态行。只看管道通断分不清"装了没起"和"压根没装"。
void DirectHostWindow::refreshSvcStateLabel() {
  if (svcConnected_) {
    return;  // 已连接文案由 onSvcInfo 负责
  }
  if (!svcInstalled_) {
    svcState_->setText(
        tr("被控服务: 未安装（本机模式推流，锁屏/UAC 不可控）"));
    svcState_->setStyleSheet("color: #8a6d3b;");
  } else if (svcRunning_) {
    svcState_->setText(
        tr("被控服务: 已安装，服务运行中，但 helper 管道未连上 —"
           "请看 rd_host_service.exe 旁 rd_debug.log 的 [service]/[helper] 行"));
    svcState_->setStyleSheet("color: #c62828;");
  } else {
    svcState_->setText(
        tr("被控服务: 已安装但服务未运行 — 点「安装被控服务」会尝试启动"));
    svcState_->setStyleSheet("color: #c62828;");
  }
}

void DirectHostWindow::onSvcStatus(bool installed, bool running) {
  svcInstalled_ = installed;
  svcRunning_ = running;
  if (!svcConnected_) {
    refreshSvcStateLabel();
    // 已安装就允许卸载，哪怕 helper 一直连不上。
    svcUninstallBtn_->setEnabled(installed);
  }
}

void DirectHostWindow::onSvcGone() {
  const bool wasConnected = svcConnected_;
  svcConnected_ = false;
  refreshSvcStateLabel();
  svcInstallBtn_->setEnabled(true);
  svcUninstallBtn_->setEnabled(svcInstalled_);
  svcStateText_.clear();
  svcStatsText_.clear();
  lockHint_->hide();
  refreshSvcStatus();
  if (wasConnected) {
    fillLocalScreens();  // 回本机模式：屏幕列表换回本进程枚举
  }
}

void DirectHostWindow::pushDefaultScreen() {
  if (!svcConnected_ || !screenBox_) {
    return;
  }
  const qint64 id = screenBox_->currentData().toLongLong();
  svc_->send("{\"type\":\"setDefaultScreen\",\"screenId\":" +
             std::to_string(id) + "}");
  rdDebug("direct-host", "default screen pushed (%lld)", (long long)id);
}

void DirectHostWindow::onSvcInstall() {
  const QString exe =
      QCoreApplication::applicationDirPath() + "/rd_host_service.exe";
  if (!QFileInfo::exists(exe)) {
    QMessageBox::warning(this, tr("远程桌面"),
                         tr("找不到 %1\n请把 rd_host_service.exe 与主程序放在"
                            "同一目录。").arg(exe));
    return;
  }
  const HINSTANCE r = ShellExecuteW(nullptr, L"runas",
                                    (const wchar_t*)exe.utf16(), L"--install",
                                    nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(r) <= 32) {
    setStatus(tr("安装被控服务被取消（需要管理员确认）"));
  } else {
    // 提权进程已拉起：装没装成以面板状态为准（helper 管道连上即就绪）。
    setStatus(tr("已发起安装，请在 UAC 弹窗确认；面板状态就绪后自动刷新"));
  }
}

void DirectHostWindow::onSvcUninstall() {
  const QString exe =
      QCoreApplication::applicationDirPath() + "/rd_host_service.exe";
  if (!QFileInfo::exists(exe)) {
    return;
  }
  const HINSTANCE r = ShellExecuteW(nullptr, L"runas",
                                    (const wchar_t*)exe.utf16(),
                                    L"--uninstall", nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(r) <= 32) {
    setStatus(tr("卸载被控服务被取消（需要管理员确认）"));
  } else {
    setStatus(tr("已发起卸载，请在 UAC 弹窗确认；面板状态稍后自动刷新"));
  }
}

}  // namespace rd