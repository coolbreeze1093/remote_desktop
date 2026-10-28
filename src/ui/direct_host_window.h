// remote_desktop — 直连被控端（rd_direct_host）主窗口。
// 信令监听有两路：
//   - 被控服务（rd_host_service，SYSTEM）：推荐。TCP 信令监听由服务本体
//     承载（session 0，不随会话切换/面板关闭消亡），面板只负责状态/统计/
//     安装，并把屏幕选择经管道推给 helper（setDefaultScreen）；
//   - 本机模式（服务未装时兜底）：面板自己监听 TCP + GUI 进程推流，
//     权限受限（锁屏不可控）。
#ifndef REMOTE_DESKTOP_UI_DIRECT_HOST_WINDOW_H_
#define REMOTE_DESKTOP_UI_DIRECT_HOST_WINDOW_H_

#include "ui/session_window.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace rd {

class DirectSignalingServer;
class HostServiceClient;

class DirectHostWindow : public SessionWindow {
  Q_OBJECT
 public:
  explicit DirectHostWindow(QWidget* parent = nullptr);

  // 命令行自动化入口：启动后立即监听指定端口（脚本/端到端测试用）。
  void autoListen(quint16 port);

 protected:
  void onSessionConnected() override;
  void onSessionDisconnected() override;

 private slots:
  void onToggleListen();
  void onSvcInstall();
  void onSvcUninstall();

 private:
  QWidget* makeHostPanel();
  void onOfferReceived(const QString& offer);
  void onClientLeft();
  void startListening();
  void stopListening();

  // 被控服务面板（与 rd_host 的 HostWindow 同一套管道客户端与事件）。
  void onSvcInfo(const QString& json);
  void onSvcEvent(const QString& json);
  void onSvcStatus(bool installed, bool running);
  void onSvcGone();
  void refreshSvcStateLabel();
  void refreshSvcStatus();
  void fillLocalScreens();
  // 服务模式：把面板选的屏幕推给 helper（startHost 缺省用它）。
  void pushDefaultScreen();

  QComboBox* screenBox_ = nullptr;
  QLineEdit* portEdit_ = nullptr;
  QPushButton* listenBtn_ = nullptr;
  QLabel* lockHint_ = nullptr;
  DirectSignalingServer* signaling_ = nullptr;
  bool listening_ = false;

  QPushButton* svcInstallBtn_ = nullptr;
  QPushButton* svcUninstallBtn_ = nullptr;
  QLabel* svcState_ = nullptr;    // 服务/连接状态行
  QLabel* svcStatus_ = nullptr;   // 会话状态 + 管道统计（有内容才显示）
  HostServiceClient* svc_ = nullptr;
  bool svcConnected_ = false;      // helper 管道已连上（服务模式生效）
  bool svcInstalled_ = false;      // SCM 探测：服务已注册
  bool svcRunning_ = false;        // SCM 探测：服务运行中
  QString svcStateText_;
  QString svcStatsText_;
};

}  // namespace rd

#endif  // REMOTE_DESKTOP_UI_DIRECT_HOST_WINDOW_H_
