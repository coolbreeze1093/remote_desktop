// remote_desktop 服务端（被控端）— rd_host.exe 主窗口。
// 本机推流（直接采集/编码/注入）+ 被控服务面板（rd_host_service，SYSTEM
// 身份，锁屏/UAC/登录界面可控）。SDP 仍为手动交换。
#ifndef REMOTE_DESKTOP_UI_HOST_WINDOW_H_
#define REMOTE_DESKTOP_UI_HOST_WINDOW_H_

#include <QString>

#include "ui/session_window.h"

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace rd {

class HostServiceClient;

class HostWindow : public SessionWindow {
  Q_OBJECT
 public:
  explicit HostWindow(QWidget* parent = nullptr);

 private slots:
  void onHostGenerateAnswer();
  void onHostToggleStream();

 private:
  QWidget* makeHostTab();
  // Host 页常驻权限状态（当前进程令牌是否提权，决定锁屏/UAC 可控性）。
  void updatePermLabel();
  // 本机输入桌面切到/离开安全桌面（锁屏/UAC）时的提示。
  void applyHostLockHint(bool secure, bool capturable);
  // 以管理员身份重启本程序（锁屏/UAC 采集与控制需要提权）。
  void onHostElevateRestart();

  // ---- 被控服务面板（SYSTEM helper：锁屏/UAC/登录界面可控）----
  void onSvcInfo(const QString& json);
  void onSvcEvent(const QString& json);
  void onSvcGone();
  void onSvcStatus(bool installed, bool running);
  void onSvcInstall();
  void onSvcUninstall();
  void onSvcStart();
  void onSvcStop();
  void refreshSvcStatus();
  // 未连接 helper 时的状态行：按 SCM 探测结果区分未安装/已停/运行中。
  void refreshSvcStateLabel();
  void setLocalHostEnabled(bool enabled);

  void onSessionConnected() override;

  // Host widgets
  QComboBox* hostScreenBox_ = nullptr;  // which monitor to capture
  QPlainTextEdit* hostOffer_ = nullptr;
  QPlainTextEdit* hostAnswer_ = nullptr;
  QPushButton* hostAnswerBtn_ = nullptr;
  QPushButton* hostStartBtn_ = nullptr;
  QLabel* hostPermLabel_ = nullptr;      // 常驻权限状态（提权与否一目了然）
  QLabel* hostLockHint_ = nullptr;      // 锁屏/提权状态提示
  QPushButton* hostElevateBtn_ = nullptr;
  int64_t hostScreenId_ = 0;
  QWidget* hostScreenLabel_ = nullptr;   // 服务模式下随本机流程一起禁用

  // 被控服务面板 widgets
  HostServiceClient* svc_ = nullptr;
  QLabel* svcState_ = nullptr;         // 与 helper 的连接状态
  QComboBox* svcScreenBox_ = nullptr;
  QPlainTextEdit* svcOffer_ = nullptr;
  QPlainTextEdit* svcAnswer_ = nullptr;
  QPushButton* svcStartBtn_ = nullptr;
  QPushButton* svcStopBtn_ = nullptr;
  QPushButton* svcInstallBtn_ = nullptr;
  QPushButton* svcUninstallBtn_ = nullptr;
  QLabel* svcStatus_ = nullptr;        // 会话状态 + 每秒统计
  QLabel* svcLockHint_ = nullptr;
  QString svcStateText_;
  QString svcStatsText_;
  bool svcConnected_ = false;
  bool svcInstalled_ = false;  // SCM 探测结果（onSvcStatus 更新）
  bool svcRunning_ = false;
  qint64 lastInjectStatusMs_ = 0;  // throttle for the input-injected status
};

}  // namespace rd

#endif
