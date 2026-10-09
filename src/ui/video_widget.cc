#include "ui/video_widget.h"

#include <QDateTime>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>

#include <windows.h>

#include <atomic>

#include "debug_log.h"

namespace rd {

namespace {

// At most ~60 move events per second are sent; the final position is flushed
// shortly after the pointer stops so slow links stay responsive.
constexpr int kMoveIntervalMs = 16;
constexpr int kMoveFlushMs = 40;

int mapQtButton(Qt::MouseButton b) {
  switch (b) {
    case Qt::LeftButton: return 1;
    case Qt::RightButton: return 2;
    case Qt::MiddleButton: return 3;
    default: return 0;
  }
}

uint32_t qtKeyToVk(int key) {
  // Letters and digits map directly (Windows VK codes mirror ASCII).
  if (key >= Qt::Key_A && key <= Qt::Key_Z) return uint32_t('A' + (key - Qt::Key_A));
  if (key >= Qt::Key_0 && key <= Qt::Key_9) return uint32_t('0' + (key - Qt::Key_0));
  switch (key) {
    case Qt::Key_Return: case Qt::Key_Enter: return VK_RETURN;
    case Qt::Key_Escape: return VK_ESCAPE;
    case Qt::Key_Tab: return VK_TAB;
    case Qt::Key_Backspace: return VK_BACK;
    case Qt::Key_Delete: return VK_DELETE;
    case Qt::Key_Insert: return VK_INSERT;
    case Qt::Key_Home: return VK_HOME;
    case Qt::Key_End: return VK_END;
    case Qt::Key_PageUp: return VK_PRIOR;
    case Qt::Key_PageDown: return VK_NEXT;
    case Qt::Key_Left: return VK_LEFT;
    case Qt::Key_Right: return VK_RIGHT;
    case Qt::Key_Up: return VK_UP;
    case Qt::Key_Down: return VK_DOWN;
    case Qt::Key_Space: return VK_SPACE;
    case Qt::Key_Control: return VK_CONTROL;
    case Qt::Key_Shift: return VK_SHIFT;
    case Qt::Key_Alt: return VK_MENU;
    case Qt::Key_Meta: return VK_LWIN;
    case Qt::Key_F1: return VK_F1;
    case Qt::Key_F2: return VK_F2;
    case Qt::Key_F3: return VK_F3;
    case Qt::Key_F4: return VK_F4;
    case Qt::Key_F5: return VK_F5;
    case Qt::Key_F6: return VK_F6;
    case Qt::Key_F7: return VK_F7;
    case Qt::Key_F8: return VK_F8;
    case Qt::Key_F9: return VK_F9;
    case Qt::Key_F10: return VK_F10;
    case Qt::Key_F11: return VK_F11;
    case Qt::Key_F12: return VK_F12;
    default: return 0;
  }
}

}  // namespace

VideoWidget::VideoWidget(QWidget* parent) : QWidget(parent) {
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setMinimumSize(320, 180);
  moveTimer_ = new QTimer(this);
  moveTimer_->setSingleShot(true);
  connect(moveTimer_, &QTimer::timeout, this, [this] {
    if (pendingMove_.x() >= 0) {
      emitMouseMove(pendingMove_);
      pendingMove_ = {-1, -1};
    }
  });
}

QRect VideoWidget::videoRect() const {
  if (frame_.isNull()) {
    return rect();
  }
  const QSize s = frame_.size().scaled(size(), Qt::KeepAspectRatio);
  const int x = (width() - s.width()) / 2;
  const int y = (height() - s.height()) / 2;
  return QRect(x, y, s.width(), s.height());
}

QPointF VideoWidget::toNormalised(const QPointF& widgetPos) const {
  const QRect r = videoRect();
  if (r.width() <= 0 || r.height() <= 0 || frame_.isNull()) {
    return {0, 0};
  }
  const double nx = (widgetPos.x() - r.left()) / r.width();
  const double ny = (widgetPos.y() - r.top()) / r.height();
  return {qBound(0.0, nx, 1.0), qBound(0.0, ny, 1.0)};
}

void VideoWidget::setFrame(const QImage& image) {
  frame_ = image;
  if (remoteW_ == 0) {
    remoteW_ = image.width();
    remoteH_ = image.height();
  }
  update();
}

void VideoWidget::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), QColor(10, 10, 14));
  if (frame_.isNull()) {
    p.setPen(QColor(120, 130, 150));
    QString text = tr("尚未收到远端画面");
    if (!diagText_.isEmpty()) {
      text += QLatin1Char('\n') + diagText_;
    }
    p.drawText(rect(), Qt::AlignCenter, text);
    return;
  }
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  p.drawImage(videoRect(), frame_);
}

void VideoWidget::emitMouseMove(const QPointF& n) {
  {
    static std::atomic<int> dbg{0};
    if (dbg.fetch_add(1) < 5) {
      rdDebug("video", "mouseMove nx=%.3f ny=%.3f", n.x(), n.y());
    }
  }
  InputEvent ev;
  ev.type = InputEvent::Type::MouseMove;
  ev.nx = n.x();
  ev.ny = n.y();
  emit inputEvent(ev);
}

void VideoWidget::mouseMoveEvent(QMouseEvent* e) {
  const QPointF n = toNormalised(e->localPos());
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (now - lastMoveMs_ >= kMoveIntervalMs) {
    lastMoveMs_ = now;
    pendingMove_ = {-1, -1};
    emitMouseMove(n);
    moveTimer_->start(kMoveFlushMs);
  } else {
    pendingMove_ = n;
  }
}

void VideoWidget::mousePressEvent(QMouseEvent* e) {
  const QPointF n = toNormalised(e->localPos());
  InputEvent ev;
  ev.type = InputEvent::Type::MouseButton;
  ev.nx = n.x();
  ev.ny = n.y();
  ev.button = mapQtButton(e->button());
  ev.pressed = true;
  emit inputEvent(ev);
}

void VideoWidget::mouseReleaseEvent(QMouseEvent* e) {
  const QPointF n = toNormalised(e->localPos());
  InputEvent ev;
  ev.type = InputEvent::Type::MouseButton;
  ev.nx = n.x();
  ev.ny = n.y();
  ev.button = mapQtButton(e->button());
  ev.pressed = false;
  emit inputEvent(ev);
}

void VideoWidget::mouseDoubleClickEvent(QMouseEvent*) {
  emit doubleClicked();
}

void VideoWidget::wheelEvent(QWheelEvent* e) {
  InputEvent ev;
  ev.type = InputEvent::Type::MouseWheel;
  ev.wheelDelta = e->angleDelta().y();
  emit inputEvent(ev);
}

void VideoWidget::keyPressEvent(QKeyEvent* e) {
  const uint32_t vk = qtKeyToVk(e->key());
  if (vk == 0) {
    QWidget::keyPressEvent(e);
    return;
  }
  InputEvent ev;
  ev.type = InputEvent::Type::Key;
  ev.keyCode = vk;
  ev.pressed = true;
  ev.extended = (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU) &&
                (e->modifiers() & (Qt::KeypadModifier));
  emit inputEvent(ev);
}

void VideoWidget::keyReleaseEvent(QKeyEvent* e) {
  const uint32_t vk = qtKeyToVk(e->key());
  if (vk == 0) {
    QWidget::keyReleaseEvent(e);
    return;
  }
  InputEvent ev;
  ev.type = InputEvent::Type::Key;
  ev.keyCode = vk;
  ev.pressed = false;
  emit inputEvent(ev);
}

}  // namespace rd
