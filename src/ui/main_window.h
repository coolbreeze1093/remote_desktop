// remote_desktop — main window: Host tab + Viewer tab.
#ifndef REMOTE_DESKTOP_UI_MAIN_WINDOW_H_
#define REMOTE_DESKTOP_UI_MAIN_WINDOW_H_

#include <QMainWindow>
#include <QPlainTextEdit>
#include <QSplitter>
#include "session/remote_desktop_session.h"
#include "ui/video_widget.h"

class QComboBox;
class QPushButton;
class QLabel;
class QTimer;

namespace rd {

class RemoteVideoWindow;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget* parent = nullptr);

 private slots:
  // Host
  void onHostGenerateAnswer();
  void onHostToggleStream();
  // Viewer
  void onViewerCreateOffer();
  void onViewerConnect();

 private:
  QWidget* makeHostTab();
  QWidget* makeViewerTab();
  void setStatus(const QString& text);
  void updateDiag();
  void sendInputEvent(const rd::InputEvent& ev);
  // 本机输入桌面切到/离开安全桌面（锁屏/UAC）时的 Host tab 提示。
  void applyHostLockHint(bool secure, bool capturable);
  // 以管理员身份重启本程序（锁屏/UAC 采集与控制需要提权）。
  void onHostElevateRestart();

  RemoteDesktopSession* session_ = nullptr;
  QTimer* diagTimer_ = nullptr;
  qint64 lastInjectStatusMs_ = 0;  // throttle for the input-injected status

  // Independent fullscreen-capable window showing the remote video.
  RemoteVideoWindow* videoWindow_ = nullptr;

  // Host widgets
  QComboBox* hostScreenBox_ = nullptr;  // which monitor to capture
  QPlainTextEdit* hostOffer_ = nullptr;
  QPlainTextEdit* hostAnswer_ = nullptr;
  QPushButton* hostAnswerBtn_ = nullptr;
  QPushButton* hostStartBtn_ = nullptr;
  QLabel* hostState_ = nullptr;
  QLabel* hostLockHint_ = nullptr;      // 锁屏/提权状态提示
  QPushButton* hostElevateBtn_ = nullptr;
  int64_t hostScreenId_ = 0;

  // Viewer widgets
  VideoWidget* viewerPreview_ = nullptr;  // embedded live preview (no popup)
  QComboBox* qualityBox_ = nullptr;       // 自动/超清/高清/流畅
  QPlainTextEdit* viewerOffer_ = nullptr;
  QPlainTextEdit* viewerAnswer_ = nullptr;
  QPushButton* viewerOfferBtn_ = nullptr;
  QPushButton* viewerConnectBtn_ = nullptr;
  QLabel* viewerState_ = nullptr;
  QLabel* viewerLockHint_ = nullptr;    // 宿主侧锁屏状态提示（操作者可见）
  QSplitter* splitter_ = nullptr;
};

}  // namespace rd

#endif
