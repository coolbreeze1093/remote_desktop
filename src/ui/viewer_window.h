// remote_desktop 客户端（主控端）— rd_viewer.exe 主窗口。
// 创建 Offer / 粘贴 Answer 建连，内嵌预览 + 可全屏的远端画面窗口，
// 鼠标键盘经数据通道转发到被控端；画质档位经控制通道下发。
#ifndef REMOTE_DESKTOP_UI_VIEWER_WINDOW_H_
#define REMOTE_DESKTOP_UI_VIEWER_WINDOW_H_

#include "ui/session_window.h"

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace rd {

class RemoteVideoWindow;
class VideoWidget;

class ViewerWindow : public SessionWindow {
  Q_OBJECT
 public:
  explicit ViewerWindow(QWidget* parent = nullptr);

 private slots:
  void onViewerCreateOffer();
  void onViewerConnect();

 private:
  QWidget* makeViewerTab();
  void sendInputEvent(const rd::InputEvent& ev);

  void onSessionConnected() override;
  void onSessionDisconnected() override;
  void applyDiagText(const QString& text) override;

  // Independent fullscreen-capable window showing the remote video.
  RemoteVideoWindow* videoWindow_ = nullptr;

  // Viewer widgets
  VideoWidget* viewerPreview_ = nullptr;  // embedded live preview (no popup)
  QComboBox* qualityBox_ = nullptr;       // 自动/超清/高清/流畅
  QPlainTextEdit* viewerOffer_ = nullptr;
  QPlainTextEdit* viewerAnswer_ = nullptr;
  QPushButton* viewerOfferBtn_ = nullptr;
  QPushButton* viewerConnectBtn_ = nullptr;
  QLabel* viewerLockHint_ = nullptr;    // 宿主侧锁屏状态提示（操作者可见）
};

}  // namespace rd

#endif
