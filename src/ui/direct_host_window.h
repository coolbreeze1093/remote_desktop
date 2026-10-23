// remote_desktop — 直连被控端（rd_direct_host）主窗口。
// 监听 TCP 端口等待主控端来连；Offer/Answer 经这条 TCP 自动交换，无需
// 手动复制粘贴，也无需信令服务器。推流/注入有两路，与 rd_host 一致：
//   - 被控服务（rd_host_service，SYSTEM）：装好后 Offer 转交 helper，
//     锁屏/UAC/登录界面均可查看与控制；
//   - 本机模式（服务未装时兜底）：GUI 进程自己推流，权限受限。
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
  // 断开正在等 Answer 的信令客户端（服务出错/掉线时快速失败，继续监听）。
  void failPendingSignaling(const QString& reason);

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
  bool svcSessionActive_ = false;  // helper 正在推流（state 事件驱动）
  bool awaitSvcAnswer_ = false;    // Offer 已交服务，等 answer 事件
  QString svcStateText_;
  QString svcStatsText_;
};

}  // namespace rd

#endif  // REMOTE_DESKTOP_UI_DIRECT_HOST_WINDOW_H_
