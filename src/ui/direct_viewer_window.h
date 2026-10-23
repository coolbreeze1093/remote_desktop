// remote_desktop — 直连主控端（rd_direct_viewer）主窗口。
// 输入被控端 IP:端口直连；Offer/Answer 经这条 TCP 自动交换后进入与
// rd_viewer 完全相同的远端画面 + 键鼠转发体验（共用 RemoteDesktopSession）。
#ifndef REMOTE_DESKTOP_UI_DIRECT_VIEWER_WINDOW_H_
#define REMOTE_DESKTOP_UI_DIRECT_VIEWER_WINDOW_H_

#include "ui/session_window.h"

class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace rd {

class DirectSignalingClient;
class RemoteVideoWindow;
class VideoWidget;

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

 private slots:
  void onToggleConnect();

 private:
  QWidget* makeViewerPanel();
  void onAnswerReceived(const QString& answer);
  void onSignalingFailed(const QString& reason);
  void sendInputEvent(const rd::InputEvent& ev);
  void resetForRetry(const QString& statusText);

  QLineEdit* hostEdit_ = nullptr;
  QLineEdit* portEdit_ = nullptr;
  QPushButton* connectBtn_ = nullptr;
  QComboBox* qualityBox_ = nullptr;
  QLabel* lockHint_ = nullptr;
  VideoWidget* viewerPreview_ = nullptr;
  RemoteVideoWindow* videoWindow_ = nullptr;
  DirectSignalingClient* signaling_ = nullptr;
  QString pendingOffer_;
  bool connecting_ = false;  // 信令交换进行中（未到 session connected）
};

}  // namespace rd

#endif  // REMOTE_DESKTOP_UI_DIRECT_VIEWER_WINDOW_H_