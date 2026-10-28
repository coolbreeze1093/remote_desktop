// remote_desktop — 直连主控端（rd_direct_viewer）主窗口。
// 输入被控端 IP:端口直连；Offer/Answer 经这条 TCP 自动交换后进入与
// rd_viewer 完全相同的远端画面 + 键鼠转发体验（共用 RemoteDesktopSession）。
#ifndef REMOTE_DESKTOP_UI_DIRECT_VIEWER_WINDOW_H_
#define REMOTE_DESKTOP_UI_DIRECT_VIEWER_WINDOW_H_

#include <QStringList>
#include <QVector>

#include "ui/session_window.h"

class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace rd {

class DirectSignalingClient;
class RemoteVideoWindow;

class DirectViewerWindow : public SessionWindow {
  Q_OBJECT
 public:
  explicit DirectViewerWindow(QWidget* parent = nullptr);

  // 命令行自动化入口：启动后立即连接（脚本/端到端测试用）。
  void autoConnect(const QString& host, quint16 port);

 protected:
  void closeEvent(QCloseEvent* event) override;
  void onSessionConnected() override;
  void onSessionDisconnected() override;
  void onSessionFailed(const QString& reason) override;

 private slots:
  void onToggleConnect();

 private:
  QWidget* makeViewerPanel();
  bool startConnection(const QString& host, quint16 port);
  void scheduleAutoReconnect(const QString& reason);
  void onAnswerReceived(const QString& answer);
  void onSignalingFailed(const QString& reason);
  void onScreensReceived(const QVector<qint64>& screenIds,
                         const QStringList& screenTitles);
  void sendOfferNow();
  void sendInputEvent(const rd::InputEvent& ev);
  void resetForRetry(const QString& statusText);

  QLineEdit* hostEdit_ = nullptr;
  QLineEdit* portEdit_ = nullptr;
  QPushButton* connectBtn_ = nullptr;
  QPushButton* showVideoBtn_ = nullptr;  // 连接中重新弹出远端画面窗口
  QComboBox* qualityBox_ = nullptr;
  QComboBox* screenBox_ = nullptr;      // 被控端屏幕（连接时自动拉列表）
  QVector<qint64> screenIds_;           // 与 screenBox_ 条目一一对应
  bool screensKnown_ = false;           // 已拿到被控端屏幕列表
  bool offerSent_ = false;              // 本次信令连接是否已发出 Offer
  QTimer* screensTimer_ = nullptr;      // 列表超时兑底（超时按默认屏发 Offer）
  QLabel* lockHint_ = nullptr;
  RemoteVideoWindow* videoWindow_ = nullptr;
  DirectSignalingClient* signaling_ = nullptr;
  QString pendingOffer_;
  bool connecting_ = false;  // 信令交换进行中（未到 session connected）

  // 被控端会话/进程随控制台会话切换而消亡（锁屏解锁、切换用户、RDP 接管）
  // 时 P2P 必断：direct-host 会自动接受新 Offer，只要主控端重连一次就能在
  // 新会话的 helper 上恢复推流。异常断开后自动重试，成功一次即清零。
  QTimer* reconnectTimer_ = nullptr;
  QString autoHost_;
  quint16 autoPort_ = 0;
  int autoAttempts_ = 0;      // 本轮已发起的自动重试次数
  bool everConnected_ = false;  // 至少成功建立过一次会话（自动重连的前提）
  bool userStopped_ = false;    // 用户主动断开/关窗：本轮不自动重连
};

}  // namespace rd

#endif  // REMOTE_DESKTOP_UI_DIRECT_VIEWER_WINDOW_H_