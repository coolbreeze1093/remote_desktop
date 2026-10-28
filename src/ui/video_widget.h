// remote_desktop — video rendering widget with input forwarding.
#ifndef REMOTE_DESKTOP_UI_VIDEO_WIDGET_H_
#define REMOTE_DESKTOP_UI_VIDEO_WIDGET_H_

#include <QImage>
#include <QObject>
#include <QPointF>
#include <QSet>
#include <QWidget>

#include "core/input_event.h"

namespace rd {

// Displays the remote frame (letterboxed) and converts Qt input events into
// InputEvents with normalised coordinates.
class VideoWidget : public QWidget {
  Q_OBJECT
 public:
  explicit VideoWidget(QWidget* parent = nullptr);

  QSize sizeHint() const override { return {960, 540}; }

  // Frame geometry of the *remote* capture (for coordinate mapping notes).
  void setRemoteGeometry(int w, int h) { remoteW_ = w; remoteH_ = h; }
  int remoteWidth() const { return remoteW_; }
  int remoteHeight() const { return remoteH_; }

  // Diagnostic line shown under the "no picture" placeholder (e.g. frame
  // counters) so a dead pipeline stage is visible on screen.
  void setDiagText(const QString& text) { diagText_ = text; update(); }

 public slots:
  void setFrame(const QImage& image);

  // 远端光标状态（include_cursor=false 后由本控件本地渲染）：位置为归一化
  // 坐标 [0..1]（相对远端帧，越界不绘制）；visible=false 隐藏。
  void setRemoteCursorPos(double nx, double ny, bool visible);
  void setRemoteCursorShape(const QImage& image, int hotspotX, int hotspotY);

 signals:
  void inputEvent(const rd::InputEvent& ev);
  // Double-click on the widget (e.g. re-open the fullscreen remote window
  // from the embedded preview).
  void doubleClicked();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void keyReleaseEvent(QKeyEvent* event) override;
  // 焦点丢失时远端不会收到物理抬起：释放所有已注入的键与鼠标键，
  // 否则远端修饰键/鼠标键永久卡住（单击变拖选、点选变多选）。
  void focusOutEvent(QFocusEvent* event) override;
  // IME 提交文本（中文等）转发到远端；预编辑串仅本地组字，不转发。
  void inputMethodEvent(QInputMethodEvent* event) override;
  QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

 private:
  QRect videoRect() const;
  QPointF toNormalised(const QPointF& widgetPos) const;
  void emitMouseMove(const QPointF& n);

  // ---- 远端注入状态跟踪（焦点丢失时按状态补齐抬起） ----
  // 把事件携带的修饰键状态与已注入状态做差量同步：缺的补按下，多的补抬起。
  // selfKey 为修饰键自身事件对应键（其按下/抬起由事件本身表达，不参与同步）。
  void syncModifiers(Qt::KeyboardModifiers mods, int selfKey = 0);
  void sendKeyEvent(uint32_t vk, bool pressed, bool extended = false);
  void sendKeyTextUnit(uint32_t utf16Unit);  // 提交文本：注入 down+up 一对
  void sendMouseButton(int button, bool pressed, double nx, double ny);
  void releaseAllInjectedInput();

  QImage frame_;
  QString diagText_;
  int remoteW_ = 0;
  int remoteH_ = 0;
  // 远端光标（本地渲染，随视频缩放）。
  QImage cursorImage_;
  int cursorHotX_ = 0;
  int cursorHotY_ = 0;
  double cursorNx_ = 0;
  double cursorNy_ = 0;
  bool cursorVisible_ = false;
  // Mouse-move throttle: at most one move per interval, plus a final move
  // when the pointer stops (keeps latency low without flooding the channel).
  qint64 lastMoveMs_ = 0;
  QPointF pendingMove_{-1, -1};
  QTimer* moveTimer_ = nullptr;
  // 远端当前“按下”的 VK 与鼠标按钮（1..5），用于焦点丢失时的补抬起。
  QSet<quint32> remoteKeysDown_;
  bool remoteButtonsDown_[6] = {};
  // 最近一次鼠标位置（归一化），焦点丢失补抬起时使用。
  double lastNx_ = 0, lastNy_ = 0;
};

}  // namespace rd

#endif
