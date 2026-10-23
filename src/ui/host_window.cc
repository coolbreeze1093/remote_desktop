#include "ui/host_window.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>

#include <vector>

#include <QComboBox>

#include <screen_capture/capture.hpp>

#include "core/input_desktop.h"
#include "debug_log.h"
#include "pipe_proto.h"
#include "service/host_service_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>  // ShellExecuteW（提权重启；LEAN_AND_MEAN 不含 shellapi）

namespace rd {

HostWindow::HostWindow(QWidget* parent) : SessionWindow(parent) {
  setCentralWidget(makeHostTab());
  // 标题带构建时间：部署副本（publish/）与构建目录的 exe 容易混跑，
  // 版本一眼可辨，避免拿旧版测出新问题。
  setWindowTitle(tr("远程桌面 被控端 — 手动 SDP 交换 (build %1 %2)").arg(
      QStringLiteral(__DATE__), QStringLiteral(__TIME__)));
  resize(1100, 720);
  setStatus(tr("就绪"));

  // 收到远端输入并注入本机时的可见确认（节流；鼠标移动可达 ~60 Hz）。
  connect(session_, &RemoteDesktopSession::inputInjected, this, [this] {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastInjectStatusMs_ >= 500) {
      lastInjectStatusMs_ = now;
      statusBar()->showMessage(tr("收到远端输入 — 已在本机注入鼠标/键盘"), 1500);
    }
  });

  // 本机锁屏/UAC 状态（采集线程触发；queued 到 UI 线程）。
  connect(session_, &RemoteDesktopSession::hostSecureDesktopChanged, this,
          &HostWindow::applyHostLockHint);

  // 被控服务面板：后台线程连 rd_host_service helper 的命名管道，事件经
  // 队列信号回到 UI 线程。服务未装/未起时面板显示"未运行"，本机流程可用。
  svc_ = new HostServiceClient(this);
  connect(svc_, &HostServiceClient::serviceInfo, this,
          &HostWindow::onSvcInfo, Qt::QueuedConnection);
  connect(svc_, &HostServiceClient::eventReceived, this,
          &HostWindow::onSvcEvent, Qt::QueuedConnection);
  connect(svc_, &HostServiceClient::serviceGone, this,
          &HostWindow::onSvcGone, Qt::QueuedConnection);
  connect(svc_, &HostServiceClient::serviceStatus, this,
          &HostWindow::onSvcStatus, Qt::QueuedConnection);
  svc_->start();
}

void HostWindow::onSessionConnected() {
  setStatus(tr("已连接 — 主控端已接入，推流中"));
}

QWidget* HostWindow::makeHostTab() {
  auto* root = new QWidget(this);
  auto* lay = new QVBoxLayout(root);

  stateLabel_ = new QLabel(
      tr("1) 选择屏幕并生成 Answer，2) 把 Answer 发给主控端"), root);
  lay->addWidget(stateLabel_);

  // ---- 被控服务面板：SYSTEM helper 承担采集/编码/注入（锁屏/UAC/登录可控）。
  // 服务未安装时显示"未运行"，下方本机流程照常可用。
  svcState_ = new QLabel(tr("被控服务: 检测中…"), root);
  svcState_->setWordWrap(true);
  lay->addWidget(svcState_);

  auto* svcInstallRow = new QHBoxLayout();
  svcInstallBtn_ = new QPushButton(tr("安装被控服务"), root);
  svcInstallBtn_->setToolTip(
      tr("把 rd_host_service 注册为 Windows 服务（SYSTEM，开机自启）。\n"
         "需要管理员确认；安装后锁屏/UAC/登录界面均可查看与控制。"));
  svcUninstallBtn_ = new QPushButton(tr("卸载被控服务"), root);
  svcUninstallBtn_->setEnabled(false);
  svcInstallRow->addWidget(svcInstallBtn_);
  svcInstallRow->addWidget(svcUninstallBtn_);
  svcInstallRow->addStretch(1);
  lay->addLayout(svcInstallRow);

  auto* svcScreenRow = new QHBoxLayout();
  svcScreenRow->addWidget(new QLabel(tr("共享的屏幕:"), root));
  svcScreenBox_ = new QComboBox(root);
  svcScreenRow->addWidget(svcScreenBox_, 1);
  lay->addLayout(svcScreenRow);

  lay->addWidget(new QLabel(tr("粘贴主控端的 Offer:"), root));
  svcOffer_ = new QPlainTextEdit(root);
  svcOffer_->setPlaceholderText(tr("在此粘贴 Offer SDP..."));
  svcOffer_->setMaximumHeight(72);
  lay->addWidget(svcOffer_);

  auto* svcOpRow = new QHBoxLayout();
  svcStartBtn_ = new QPushButton(tr("提交给服务（生成 Answer 并推流）"), root);
  svcStopBtn_ = new QPushButton(tr("停止推流"), root);
  svcStartBtn_->setEnabled(false);
  svcStopBtn_->setEnabled(false);
  svcOpRow->addWidget(svcStartBtn_);
  svcOpRow->addWidget(svcStopBtn_);
  lay->addLayout(svcOpRow);

  svcStatus_ = new QLabel(root);
  svcStatus_->setWordWrap(true);
  svcStatus_->hide();
  lay->addWidget(svcStatus_);
  svcLockHint_ = new QLabel(root);
  svcLockHint_->setWordWrap(true);
  svcLockHint_->hide();
  lay->addWidget(svcLockHint_);

  lay->addWidget(new QLabel(tr("Answer（复制发给主控端）:"), root));
  svcAnswer_ = new QPlainTextEdit(root);
  svcAnswer_->setReadOnly(true);
  svcAnswer_->setMaximumHeight(88);
  lay->addWidget(svcAnswer_);

  lay->addWidget(new QLabel(
      tr("—— 本机模式（不走服务；锁屏/UAC/登录界面不可控）——"), root));

  auto* screenRow = new QHBoxLayout();
  hostScreenLabel_ = new QLabel(tr("共享的屏幕:"), root);
  screenRow->addWidget(hostScreenLabel_);
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

  // 常驻权限状态 + 锁屏/UAC（安全桌面）提示与提权入口：未提权进程打不开
  // 安全桌面，锁屏后画面与控制中断；提权后 GDI 可捕获锁屏、SendInput 可
  // 注入。权限状态常驻显示，避免"到底有没有提权成功"说不清。
  hostPermLabel_ = new QLabel(root);
  hostPermLabel_->setWordWrap(true);
  updatePermLabel();
  lay->addWidget(hostPermLabel_);
  hostLockHint_ = new QLabel(root);
  hostLockHint_->setWordWrap(true);
  hostLockHint_->hide();
  lay->addWidget(hostLockHint_);
  hostElevateBtn_ = new QPushButton(tr("以管理员身份重启"), root);
  hostElevateBtn_->setToolTip(
      tr("锁屏/UAC 时的采集与控制需要管理员权限；重启后需重新交换 SDP。"));
  connect(hostElevateBtn_, &QPushButton::clicked, this,
          &HostWindow::onHostElevateRestart);
  lay->addWidget(hostElevateBtn_);

  lay->addWidget(new QLabel(tr("你的 Answer（复制发给主控端）:"), root));
  hostAnswer_ = new QPlainTextEdit(root);
  hostAnswer_->setReadOnly(true);
  lay->addWidget(hostAnswer_, 3);

  connect(hostAnswerBtn_, &QPushButton::clicked, this,
          &HostWindow::onHostGenerateAnswer);
  connect(hostStartBtn_, &QPushButton::clicked, this,
          &HostWindow::onHostToggleStream);

  connect(svcInstallBtn_, &QPushButton::clicked, this,
          &HostWindow::onSvcInstall);
  connect(svcUninstallBtn_, &QPushButton::clicked, this,
          &HostWindow::onSvcUninstall);
  connect(svcStartBtn_, &QPushButton::clicked, this, &HostWindow::onSvcStart);
  connect(svcStopBtn_, &QPushButton::clicked, this, &HostWindow::onSvcStop);
  return root;
}

void HostWindow::onHostGenerateAnswer() {
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

void HostWindow::onHostToggleStream() {
  // Capture loop already runs; this reserves space for pause/resume.
  setStatus(tr("推流中 — 等待主控端连接"));
}

void HostWindow::updatePermLabel() {
  if (!hostPermLabel_) {
    return;
  }
  if (rd::CanOpenSecureDesktop()) {
    hostPermLabel_->setText(
        tr("当前权限：管理员 — 锁屏/UAC 期间可查看与控制"));
    hostPermLabel_->setStyleSheet("color: #2e7d32;");
  } else {
    hostPermLabel_->setText(
        tr("当前权限：普通 — 锁屏/UAC 期间无法查看与控制；需要时点下方"
           "「以管理员身份重启」"));
    hostPermLabel_->setStyleSheet("color: #c62828;");
  }
}

void HostWindow::applyHostLockHint(bool secure, bool capturable) {
  if (!hostLockHint_) {
    return;
  }
  if (!secure) {
    // 未锁屏：提示隐藏；权限状况由上方常驻的权限标签负责展示。
    hostLockHint_->hide();
    return;
  }
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
}

void HostWindow::onHostElevateRestart() {
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

// ---------- 被控服务面板 ----------

void HostWindow::setLocalHostEnabled(bool enabled) {
  QWidget* const widgets[] = {hostScreenLabel_, hostScreenBox_, hostOffer_,
                              hostAnswerBtn_,   hostStartBtn_,   hostAnswer_,
                              hostElevateBtn_};
  for (QWidget* w : widgets) {
    if (w) {
      w->setEnabled(enabled);
    }
  }
}

void HostWindow::refreshSvcStatus() {
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

void HostWindow::onSvcInfo(const QString& json) {
  const std::string j = json.toStdString();
  const std::string version = rdpipe::JsonGetString(j, "version");
  const long elevated = rdpipe::JsonGetInt(j, "elevated");
  std::vector<long long> ids;
  std::vector<std::string> titles;
  rdpipe::JsonGetArrayInt(j, "screenIds", &ids);
  rdpipe::JsonGetArrayString(j, "screenTitles", &titles);

  svcConnected_ = true;
  svcScreenBox_->clear();
  for (size_t i = 0; i < titles.size(); ++i) {
    const long long id = i < ids.size() ? ids[i] : (long long)i;
    svcScreenBox_->addItem(QString::fromStdString(titles[i]),
                           QVariant::fromValue<qint64>(id));
  }
  if (svcScreenBox_->count() == 0) {
    svcScreenBox_->addItem(tr("屏幕 0"), QVariant::fromValue<qint64>(0));
  }
  svcState_->setText(
      tr("被控服务: 已连接 helper（%1%2）")
          .arg(QString::fromStdString(version))
          .arg(elevated ? tr("，SYSTEM 身份") : tr("，非 SYSTEM（异常）")));
  svcState_->setStyleSheet(elevated ? "color: #2e7d32;" : "color: #c62828;");
  svcStartBtn_->setEnabled(true);
  svcInstallBtn_->setEnabled(false);
  svcUninstallBtn_->setEnabled(true);
  setLocalHostEnabled(false);
  rdDebug("mainwin", "service helper connected (%s)", version.c_str());
}

void HostWindow::onSvcEvent(const QString& json) {
  const std::string j = json.toStdString();
  const std::string type = rdpipe::JsonGetString(j, "type");
  if (type == "answer") {
    svcAnswer_->setPlainText(
        QString::fromStdString(rdpipe::JsonGetString(j, "sdp")));
    setStatus(tr("服务已生成 Answer — 复制给主控端，连接后自动推流"));
    rdDebug("mainwin", "service answer received");
  } else if (type == "state") {
    svcStateText_ = tr("会话状态: %1")
                        .arg(QString::fromStdString(
                            rdpipe::JsonGetString(j, "state")));
    const std::string detail = rdpipe::JsonGetString(j, "detail");
    if (!detail.empty()) {
      svcStateText_ += tr("（%1）").arg(QString::fromStdString(detail));
    }
    const bool closed = rdpipe::JsonGetString(j, "state") == "closed";
    svcStopBtn_->setEnabled(svcConnected_ && !closed);
    refreshSvcStatus();
  } else if (type == "secure") {
    const bool secure = rdpipe::JsonGetInt(j, "secure") != 0;
    const bool capturable = rdpipe::JsonGetInt(j, "capturable") != 0;
    if (!secure) {
      svcLockHint_->hide();
    } else if (capturable) {
      svcLockHint_->setText(
          tr("已锁屏/UAC — 服务（SYSTEM）正在捕获安全桌面，画面与输入正常"));
      svcLockHint_->setStyleSheet("color: #2e7d32;");
      svcLockHint_->show();
    } else {
      svcLockHint_->setText(
          tr("已锁屏/UAC — 服务未能打开安全桌面（异常），请回报 rd_debug.log"));
      svcLockHint_->setStyleSheet("color: #c62828;");
      svcLockHint_->show();
    }
  } else if (type == "error") {
    svcStateText_ = tr("错误: %1")
                        .arg(QString::fromStdString(
                            rdpipe::JsonGetString(j, "msg")));
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

// 未连接 helper 时的状态行。此前只看管道通断，"服务装了但 helper 没起来"
// 与"压根没装"都显示"未运行"，安装过没效果时分不清该查哪一头。
void HostWindow::refreshSvcStateLabel() {
  if (svcConnected_) {
    return;  // 已连接文案由 onSvcInfo 负责
  }
  if (!svcInstalled_) {
    svcState_->setText(tr("被控服务: 未安装"));
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

void HostWindow::onSvcStatus(bool installed, bool running) {
  svcInstalled_ = installed;
  svcRunning_ = running;
  if (!svcConnected_) {
    refreshSvcStateLabel();
    // 已安装就允许卸载，哪怕 helper 一直连不上（此前只有连上才能卸载）。
    svcUninstallBtn_->setEnabled(installed);
  }
}

void HostWindow::onSvcGone() {
  svcConnected_ = false;
  refreshSvcStateLabel();
  svcStartBtn_->setEnabled(false);
  svcStopBtn_->setEnabled(false);
  svcInstallBtn_->setEnabled(true);
  svcUninstallBtn_->setEnabled(svcInstalled_);
  setLocalHostEnabled(true);
  svcStateText_.clear();
  svcStatsText_.clear();
  svcLockHint_->hide();
  refreshSvcStatus();
}

void HostWindow::onSvcInstall() {
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

void HostWindow::onSvcUninstall() {
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

void HostWindow::onSvcStart() {
  const QString offer = svcOffer_->toPlainText().trimmed();
  if (!svcConnected_ || offer.isEmpty()) {
    QMessageBox::information(this, tr("远程桌面"),
                             tr("请先粘贴主控端的 Offer"));
    return;
  }
  const qint64 screenId = svcScreenBox_->currentData().toLongLong();
  const std::string json =
      "{\"type\":\"startHost\",\"screenId\":" + std::to_string(screenId) +
      ",\"offer\":" + rdpipe::JStr(offer.toStdString()) + "}";
  svc_->send(json);
  setStatus(tr("已把 Offer 提交给被控服务，等待 Answer…"));
}

void HostWindow::onSvcStop() { svc_->send("{\"type\":\"stopHost\"}"); }

}  // namespace rd
