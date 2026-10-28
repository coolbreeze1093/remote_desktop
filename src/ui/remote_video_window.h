// remote_desktop — independent top-level window for the remote video.
// Shows fullscreen once the session connects, with an auto-hiding floating
// toolbar (fullscreen toggle + disconnect) overlaid on the video.
#ifndef REMOTE_DESKTOP_UI_REMOTE_VIDEO_WINDOW_H_
#define REMOTE_DESKTOP_UI_REMOTE_VIDEO_WINDOW_H_

#include <QImage>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "core/input_event.h"
#include "ui/video_widget.h"

class QComboBox;
class QLabel;
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

  // 远端光标状态（转发给内嵌 VideoWidget 本地渲染）。
  void setRemoteCursorPos(double nx, double ny, bool visible) {
    video_->setRemoteCursorPos(nx, ny, visible);
  }
  void setRemoteCursorShape(const QImage& image, int hotspotX, int hotspotY) {
    video_->setRemoteCursorShape(image, hotspotX, hotspotY);
  }

  // Diagnostic line shown while no frame has been displayed yet.
  void setDiagText(const QString& text);

  // ---- 会话控制控件（画质 / 被控端屏幕）----
  // 用户在工具栏切换画质档位（0=自动 1=超清 2=高清 3=流畅）。
  int currentQualityMode() const;
  // 填充被控端屏幕列表（保留当前选择；空列表保持占位项）。
  void setScreens(const QVector<qint64>& ids, const QStringList& titles);
  // 当前选中的屏幕 id（0 = 被控端默认）。
  qint64 currentScreenId() const;
  // 当前选中的屏幕名称（状态栏展示用；未知时返回占位文案）。
  QString screenTitle() const;
  // 屏幕选择是否可见（rd_viewer 的屏幕由被控端选，隐藏此项）。
  void setScreenSelectorVisible(bool visible);

  // Pops the window up fullscreen (used when the session connects).
  void showFullscreen();

  // 全屏/窗口切换（工具栏按钮与预览双击共用）。
  void toggleFullscreen();

 signals:
  // Forwarded from the inner VideoWidget.
  void inputEvent(const rd::InputEvent& ev);
  // User clicked "断开连接" or closed the window.
  void disconnectRequested();
  // 用户切换画质档位（主窗口接到后转发给会话）。
  void qualityModeSelected(int mode);
  // 用户切换被控端屏幕（主窗口决定如何生效：直连=换屏重连）。
  void screenSelected(qint64 screenId);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void changeEvent(QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void closeEvent(QCloseEvent* event) override;

 private:
  void buildToolbar();
  void showToolbar();
  void positionToolbar();

  VideoWidget* video_ = nullptr;
  QWidget* toolbar_ = nullptr;
  QComboBox* cursorCombo_ = nullptr;
  QComboBox* qualityCombo_ = nullptr;
  QComboBox* screenCombo_ = nullptr;
  QLabel* qualityLabel_ = nullptr;
  QLabel* screenLabel_ = nullptr;
  QPushButton* fullscreenBtn_ = nullptr;
  QPushButton* disconnectBtn_ = nullptr;
  QTimer* hideTimer_ = nullptr;
};

}  // namespace rd

#endif
