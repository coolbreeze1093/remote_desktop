// remote_desktop — video rendering widget with input forwarding.
#ifndef REMOTE_DESKTOP_UI_VIDEO_WIDGET_H_
#define REMOTE_DESKTOP_UI_VIDEO_WIDGET_H_

#include <QImage>
#include <QObject>
#include <QPointF>
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

 private:
  QRect videoRect() const;
  QPointF toNormalised(const QPointF& widgetPos) const;
  void emitMouseMove(const QPointF& n);

  QImage frame_;
  QString diagText_;
  int remoteW_ = 0;
  int remoteH_ = 0;
  // Mouse-move throttle: at most one move per interval, plus a final move
  // when the pointer stops (keeps latency low without flooding the channel).
  qint64 lastMoveMs_ = 0;
  QPointF pendingMove_{-1, -1};
  QTimer* moveTimer_ = nullptr;
};

}  // namespace rd

#endif
