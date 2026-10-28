#include "ui/video_widget.h"

#include <QDateTime>
#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QVariant>
#include <QWheelEvent>

#include <windows.h>

#include <atomic>

#include "debug_log.h"

namespace rd {

namespace {

// At most one move event per kMoveIntervalMs is sent; the final position is
// flushed shortly after the pointer stops so the remote cursor lands exactly.
constexpr int kMoveIntervalMs = 10;
constexpr int kMoveFlushMs = 40;
// 本地预测有效期：操作后这段时间内的宿主回报视作“过去的位置”丢弃，
// 停稳后回报才用于校准落点（修正注入失败/坐标截断等漂移）。
constexpr int kPredictionHoldMs = 150;

int mapQtButton(Qt::MouseButton b) {
  switch (b) {
    case Qt::LeftButton: return 1;
    case Qt::RightButton: return 2;
    case Qt::MiddleButton: return 3;
    case Qt::XButton1: return 4;  // 鼠标后退键
    case Qt::XButton2: return 5;  // 鼠标前进键
    default: return 0;
  }
}

// Qt 键 → Windows VK。要点：
//  - 标点按美式布局映射 VK_OEM_*（远端布局不同时字符跟随远端布局，与物理
//    键盘转发行为一致）。
//  - 小键盘（KeypadModifier）：NumLock 开时数字/运算符映射 VK_NUMPAD*；
//    NumLock 关时导航键映射为不带扩展位的小键盘 VK_*。
//  - 导航键（方向/Home/End/PgUp/PgDn/Insert/Delete）在主键区是扩展键，必须
//    带 KEYEVENTF_EXTENDEDKEY，否则远端当成小键盘键（NumLock 开时
//    Delete→'.'、Left→'4'，终端/编辑器里表现为按键失灵）。
uint32_t qtKeyToVk(int key, Qt::KeyboardModifiers mods, bool* extended) {
  const bool keypad = (mods & Qt::KeypadModifier) != 0;
  if (extended) {
    *extended = false;
  }
  // Letters and digits map directly (Windows VK codes mirror ASCII).
  if (key >= Qt::Key_A && key <= Qt::Key_Z) return uint32_t('A' + (key - Qt::Key_A));
  if (key >= Qt::Key_0 && key <= Qt::Key_9) {
    if (keypad) {
      return uint32_t(VK_NUMPAD0 + (key - Qt::Key_0));
    }
    return uint32_t('0' + (key - Qt::Key_0));
  }
  if (keypad) {
    // NumLock 开：小键盘运算符（小数点/逗号在小键盘上是 Del 位）。
    switch (key) {
      case Qt::Key_Slash: return VK_DIVIDE;
      case Qt::Key_Asterisk: return VK_MULTIPLY;
      case Qt::Key_Minus: return VK_SUBTRACT;
      case Qt::Key_Plus: return VK_ADD;
      case Qt::Key_Period:
      case Qt::Key_Comma: return VK_DECIMAL;
      default: break;
    }
  }
  switch (key) {
    case Qt::Key_Return: return VK_RETURN;  // 主键区回车不是扩展键
    case Qt::Key_Enter:
      // 小键盘回车是扩展键。
      if (extended) *extended = true;
      return VK_RETURN;
    case Qt::Key_Escape: return VK_ESCAPE;
    case Qt::Key_Tab: return VK_TAB;
    case Qt::Key_Backspace: return VK_BACK;
    // 导航键：主键区变体为扩展键；小键盘变体（NumLock 关）不带扩展位。
    case Qt::Key_Insert: case Qt::Key_Delete: case Qt::Key_Home:
    case Qt::Key_End: case Qt::Key_PageUp: case Qt::Key_PageDown:
    case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up:
    case Qt::Key_Down: {
      if (extended) *extended = !keypad;
      switch (key) {
        case Qt::Key_Insert: return VK_INSERT;
        case Qt::Key_Delete: return VK_DELETE;
        case Qt::Key_Home: return VK_HOME;
        case Qt::Key_End: return VK_END;
        case Qt::Key_PageUp: return VK_PRIOR;
        case Qt::Key_PageDown: return VK_NEXT;
        case Qt::Key_Left: return VK_LEFT;
        case Qt::Key_Right: return VK_RIGHT;
        case Qt::Key_Up: return VK_UP;
        default: return VK_DOWN;
      }
    }
    // 标点与符号（US 布局 VK_OEM_*；Shift 组合出的字符由修饰键同步完成）。
    case Qt::Key_Semicolon: return VK_OEM_1;
    case Qt::Key_Equal: return VK_OEM_PLUS;
    case Qt::Key_Comma: return VK_OEM_COMMA;
    case Qt::Key_Minus: return VK_OEM_MINUS;
    case Qt::Key_Period: return VK_OEM_PERIOD;
    case Qt::Key_Slash: return VK_OEM_2;
    case Qt::Key_QuoteLeft: return VK_OEM_3;
    case Qt::Key_BracketLeft: return VK_OEM_4;
    case Qt::Key_Backslash: return VK_OEM_5;
    case Qt::Key_BracketRight: return VK_OEM_6;
    case Qt::Key_Apostrophe: return VK_OEM_7;
    case Qt::Key_Space: return VK_SPACE;
    case Qt::Key_Control: return VK_CONTROL;
    case Qt::Key_Shift: return VK_SHIFT;
    case Qt::Key_Alt: return VK_MENU;
    case Qt::Key_Meta: return VK_LWIN;
    case Qt::Key_CapsLock: return VK_CAPITAL;
    case Qt::Key_NumLock: return VK_NUMLOCK;
    case Qt::Key_ScrollLock: return VK_SCROLL;
    case Qt::Key_Print: return VK_SNAPSHOT;
    case Qt::Key_Pause: return VK_PAUSE;
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
  // 让 IME 在本控件上激活（中文等输入法提交时才有 inputMethodEvent）。
  setAttribute(Qt::WA_InputMethodEnabled, true);
  // 隐藏本地光标：远端光标由 paintEvent 按宿主上报的位置/形态绘制，
  // 本地箭头叠加在上面会导致"双光标"且形态永远与远端对不上。
  setCursor(Qt::BlankCursor);
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

void VideoWidget::setRemoteCursorPos(double nx, double ny, bool visible) {
  // 本地操作活跃期内丢弃宿主回报：回报走完整条网络往返，位置是滞后的，
  // 和本地预测叠加会让光标来回跳；停稳 kPredictionHoldMs 后恢复采用，
  // 由宿主真实位置校准落点。
  if (QDateTime::currentMSecsSinceEpoch() - lastLocalInputMs_ <
      kPredictionHoldMs) {
    return;
  }
  cursorNx_ = nx;
  cursorNy_ = ny;
  cursorVisible_ = visible;
  update();
}

void VideoWidget::predictLocalCursor(const QPointF& n) {
  // 本地预测渲染：光标立即画在本地指针位置，不等「发送→注入→回报」一圈
  // 回来（那会背上整个网络往返延迟，表现为光标滞后于手）。注入坐标用的
  // 就是同一个归一化位置，预测与远端真实位置一致；漂移由停稳后的宿主
  // 回报校准。
  lastLocalInputMs_ = QDateTime::currentMSecsSinceEpoch();
  cursorNx_ = n.x();
  cursorNy_ = n.y();
  cursorVisible_ = true;
  update();
}

void VideoWidget::setRemoteCursorShape(const QImage& image, int hotspotX,
                                       int hotspotY) {
  cursorImage_ = image;
  cursorHotX_ = hotspotX;
  cursorHotY_ = hotspotY;
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
  const QRect r = videoRect();
  p.drawImage(r, frame_);
  // 远端光标叠加绘制（宿主侧不再把光标烤进视频）：形态始终按原生像素
  // 1:1 绘制（大小与被控端屏幕上一致，不随视频缩放/全屏窗口放大），
  // 位置仍按视频缩放比映射到画面上，越界（多屏时光标在采集屏外）不画。
  if (cursorVisible_ && !cursorImage_.isNull()) {
    if (cursorNx_ >= 0.0 && cursorNx_ <= 1.0 && cursorNy_ >= 0.0 &&
        cursorNy_ <= 1.0) {
      p.drawImage(
          QPointF(r.left() + cursorNx_ * r.width() - cursorHotX_,
                  r.top() + cursorNy_ * r.height() - cursorHotY_),
          cursorImage_);
    }
  }
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
  lastNx_ = n.x();
  lastNy_ = n.y();
  predictLocalCursor(n);
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
  lastNx_ = n.x();
  lastNy_ = n.y();
  predictLocalCursor(n);
  // Ctrl+click 多选 / Shift+click 范围选择等组合先差量同步修饰键。
  syncModifiers(e->modifiers());
  sendMouseButton(mapQtButton(e->button()), true, n.x(), n.y());
}

void VideoWidget::mouseReleaseEvent(QMouseEvent* e) {
  const QPointF n = toNormalised(e->localPos());
  lastNx_ = n.x();
  lastNy_ = n.y();
  predictLocalCursor(n);
  syncModifiers(e->modifiers());
  sendMouseButton(mapQtButton(e->button()), false, n.x(), n.y());
}

void VideoWidget::mouseDoubleClickEvent(QMouseEvent* e) {
  // Qt 双击序列：Press, Release, DblClick（代替第二次 Press）, Release。
  // 若不在这里补发，远端只收到 down/up/up，Windows 合不成 WM_LBUTTONDBLCLK，
  // 被控端表现为"双击无反应"。补发后远端收到完整 down/up/down/up。
  const QPointF n = toNormalised(e->localPos());
  lastNx_ = n.x();
  lastNy_ = n.y();
  predictLocalCursor(n);
  syncModifiers(e->modifiers());
  sendMouseButton(mapQtButton(e->button()), true, n.x(), n.y());

  // 本地行为不变：双击预览画面弹出/收起全屏远端窗口。
  emit doubleClicked();
}

void VideoWidget::wheelEvent(QWheelEvent* e) {
  // Ctrl+滚轮缩放等组合先差量同步修饰键。
  syncModifiers(e->modifiers());
  const QPoint delta = e->angleDelta();
  if (delta.y() != 0) {
    InputEvent ev;
    ev.type = InputEvent::Type::MouseWheel;
    ev.wheelDelta = delta.y();
    emit inputEvent(ev);
  }
  // 触摸板横向滚动 / 倾斜滚轮：远端注入 MOUSEEVENTF_HWHEEL。
  if (delta.x() != 0) {
    InputEvent ev;
    ev.type = InputEvent::Type::MouseHWheel;
    ev.wheelDelta = delta.x();
    emit inputEvent(ev);
  }
  e->accept();
}

void VideoWidget::keyPressEvent(QKeyEvent* e) {
  // 组合键差量同步修饰键：Ctrl+C 的事件是"Key_C + ControlModifier"，只发 C
  // 的话远端收到裸 'c'，快捷键全部失效。修饰键自身的事件由事件本身表达
  // （syncModifiers 的 selfKey 跳过）。
  syncModifiers(e->modifiers(), e->key());
  bool extended = false;
  const uint32_t vk = qtKeyToVk(e->key(), e->modifiers(), &extended);
  if (vk != 0 && (e->key() == Qt::Key_Control || e->key() == Qt::Key_Alt)) {
    // 右 Ctrl / 右 Alt 是扩展键；Qt 的 key 不区分左右，查原始扫描码扩展位
    // （Qt 文档：Windows 上左 Ctrl=29、右 Ctrl=285，扩展位记在 0x100）。
    extended = (e->nativeScanCode() & 0x100) != 0;
  }
  if (vk == 0) {
    QWidget::keyPressEvent(e);
    return;
  }
  // 按住不放的自动重复会连续到达这里：连续注入 down，远端即得到连续字符，
  // 与本地打字体验一致。
  sendKeyEvent(vk, true, extended);
}

void VideoWidget::keyReleaseEvent(QKeyEvent* e) {
  syncModifiers(e->modifiers(), e->key());
  bool extended = false;
  const uint32_t vk = qtKeyToVk(e->key(), e->modifiers(), &extended);
  if (vk == 0) {
    QWidget::keyReleaseEvent(e);
    return;
  }
  sendKeyEvent(vk, false, extended);
}

void VideoWidget::focusOutEvent(QFocusEvent* e) {
  // 焦点丢失（点别的窗口/Alt+Tab）后收不到物理抬起：按跟踪状态补齐抬起，
  // 否则远端修饰键/鼠标键永久卡住（单击变拖选、点选变多选）。
  releaseAllInjectedInput();
  QWidget::focusOutEvent(e);
}

void VideoWidget::inputMethodEvent(QInputMethodEvent* e) {
  // 中文等 IME 输入只在提交时转发；预编辑串是本地组字状态，远端没有对应
  // UI，不转发。每个 UTF-16 码元注入一对 down/up（VK_PACKET 路径），代理对
  // 自然拆成高低半字，远端重新组合成完整字符。
  const QString commit = e->commitString();
  for (QChar ch : commit) {
    sendKeyTextUnit(ch.unicode());
  }
  e->accept();
}

QVariant VideoWidget::inputMethodQuery(Qt::InputMethodQuery query) const {
  if (query == Qt::ImEnabled) {
    return true;
  }
  return QWidget::inputMethodQuery(query);
}

void VideoWidget::syncModifiers(Qt::KeyboardModifiers mods, int selfKey) {
  struct ModDef {
    Qt::KeyboardModifier qm;
    int qtKey;
    uint32_t vk;
  };
  static const ModDef kMods[] = {
      {Qt::ShiftModifier, Qt::Key_Shift, VK_SHIFT},
      {Qt::ControlModifier, Qt::Key_Control, VK_CONTROL},
      {Qt::AltModifier, Qt::Key_Alt, VK_MENU},
  };
  for (const ModDef& m : kMods) {
    if (m.qtKey == selfKey) {
      continue;  // 修饰键自身的按下/抬起由它自己的事件表达
    }
    const bool want = (mods & m.qm) != 0;
    const bool have = remoteKeysDown_.contains(m.vk);
    if (want && !have) {
      sendKeyEvent(m.vk, true);
    } else if (!want && have) {
      sendKeyEvent(m.vk, false);
    }
  }
}

void VideoWidget::sendKeyEvent(uint32_t vk, bool pressed, bool extended) {
  InputEvent ev;
  ev.type = InputEvent::Type::Key;
  ev.keyCode = vk;
  ev.pressed = pressed;
  ev.extended = extended;
  if (pressed) {
    remoteKeysDown_.insert(vk);
  } else {
    remoteKeysDown_.remove(vk);
  }
  emit inputEvent(ev);
}

void VideoWidget::sendKeyTextUnit(uint32_t utf16Unit) {
  for (int pass = 0; pass < 2; ++pass) {
    InputEvent ev;
    ev.type = InputEvent::Type::KeyText;
    ev.keyCode = utf16Unit;
    ev.pressed = (pass == 0);
    emit inputEvent(ev);
  }
}

void VideoWidget::sendMouseButton(int button, bool pressed, double nx,
                                  double ny) {
  if (button < 1 || button > 5) {
    return;
  }
  InputEvent ev;
  ev.type = InputEvent::Type::MouseButton;
  ev.nx = nx;
  ev.ny = ny;
  ev.button = button;
  ev.pressed = pressed;
  remoteButtonsDown_[button] = pressed;
  emit inputEvent(ev);
}

void VideoWidget::releaseAllInjectedInput() {
  // 顺序：先普通键后修饰键（修饰键最后松开，与物理释放习惯一致）。
  // 遍历用的是副本：sendKeyEvent 会修改 remoteKeysDown_，直接迭代会失效。
  static const uint32_t kModVks[] = {VK_SHIFT, VK_CONTROL, VK_MENU};
  const auto isMod = [](uint32_t vk) {
    for (const uint32_t m : kModVks) {
      if (vk == m) {
        return true;
      }
    }
    return false;
  };
  const QSet<quint32> down = remoteKeysDown_;
  remoteKeysDown_.clear();
  for (const quint32 vk : down) {
    if (!isMod(vk)) {
      sendKeyEvent(vk, false);
    }
  }
  for (const uint32_t m : kModVks) {
    if (down.contains(m)) {
      sendKeyEvent(m, false);
    }
  }
  for (int b = 1; b <= 5; ++b) {
    if (remoteButtonsDown_[b]) {
      sendMouseButton(b, false, lastNx_, lastNy_);
    }
  }
}

}  // namespace rd
