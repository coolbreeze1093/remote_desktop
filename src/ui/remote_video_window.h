// remote_desktop — independent top-level window for the remote video.
// Shows fullscreen once the session connects, with an auto-hiding floating
// toolbar (fullscreen toggle + disconnect) overlaid on the video.
#ifndef REMOTE_DESKTOP_UI_REMOTE_VIDEO_WINDOW_H_
#define REMOTE_DESKTOP_UI_REMOTE_VIDEO_WINDOW_H_

#include <QImage>
#include <QWidget>

#include "core/input_event.h"
#include "ui/video_widget.h"

class QPushButton;
class QTimer;

namespace rd {

class RemoteVideoWindow : public QWidget {
  Q_OBJECT
 public:
  explicit RemoteVideoWindow(QWidget* parent = nullptr);

 public slots:
  // Displays one decoded remote frame.
  void setFrame(const QImage& image);

  // Diagnostic line shown while no frame has been displayed yet.
  void setDiagText(const QString& text);

  // Pops the window up fullscreen (used when the session connects).
  void showFullscreen();

 signals:
  // Forwarded from the inner VideoWidget.
  void inputEvent(const rd::InputEvent& ev);
  // User clicked "断开连接" or closed the window.
  void disconnectRequested();

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void changeEvent(QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void closeEvent(QCloseEvent* event) override;

 private:
  void buildToolbar();
  void toggleFullscreen();
  void showToolbar();
  void positionToolbar();

  VideoWidget* video_ = nullptr;
  QWidget* toolbar_ = nullptr;
  QPushButton* fullscreenBtn_ = nullptr;
  QPushButton* disconnectBtn_ = nullptr;
  QTimer* hideTimer_ = nullptr;
};

}  // namespace rd

#endif
