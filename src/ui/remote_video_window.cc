#include "ui/remote_video_window.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <QVector>

#include "../debug_log.h"

namespace rd {

namespace {
// The toolbar reappears when the cursor comes within this distance of the
// top edge, and hides itself after this much idle time.
constexpr int kTopHotZone = 60;
constexpr int kToolbarHideMs = 3000;
}  // namespace

RemoteVideoWindow::RemoteVideoWindow(QWidget* parent)
    : QWidget(parent, Qt::Window) {
  setWindowTitle(tr("远程桌面 — 远端画面"));
  resize(1100, 720);

  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  video_ = new VideoWidget(this);
  lay->addWidget(video_);
  video_->installEventFilter(this);

  buildToolbar();

  hideTimer_ = new QTimer(this);
  hideTimer_->setSingleShot(true);
  connect(hideTimer_, &QTimer::timeout, this, [this] { toolbar_->hide(); });

  connect(video_, &VideoWidget::inputEvent, this,
          &RemoteVideoWindow::inputEvent);
}

void RemoteVideoWindow::buildToolbar() {
  toolbar_ = new QWidget(this);
  toolbar_->setAttribute(Qt::WA_StyledBackground, true);
  toolbar_->setStyleSheet(
      "QWidget { background: rgba(18, 20, 26, 205); border-radius: 10px; }"
      "QPushButton { color: white; background: rgba(62, 66, 76, 210);"
      "  border: none; border-radius: 6px; padding: 6px 16px; }"
      "QPushButton:hover { background: rgba(96, 102, 114, 230); }"
      "QPushButton#disconnectBtn { background: rgba(150, 52, 52, 220); }"
      "QPushButton#disconnectBtn:hover { background: rgba(190, 70, 70, 240); }"
      "QLabel { color: white; background: transparent; }"
      "QComboBox { color: white; background: rgba(62, 66, 76, 210);"
      "  border: none; border-radius: 6px; padding: 4px 8px; }"
      "QComboBox QAbstractItemView { background: #262a32; color: white;"
      "  selection-background-color: #3e424c; }");
  auto* lay = new QHBoxLayout(toolbar_);
  lay->setContentsMargins(12, 6, 12, 6);
  lay->setSpacing(10);
  // 光标大小：1.0 = 与本地系统光标等大，默认 0.8（观感更自然）；
  // 全局生效并持久化，预览与全屏窗口同步。
  auto* cursorLabel = new QLabel(tr("光标"), toolbar_);
  cursorCombo_ = new QComboBox(toolbar_);
  const QList<double> scales = {0.6, 0.8, 1.0, 1.25};
  const QStringList names = {"60%", "80%", "100%", "125%"};
  int cur = 1;
  for (int i = 0; i < scales.size(); ++i) {
    cursorCombo_->addItem(names[i], scales[i]);
    if (qAbs(scales[i] - VideoWidget::cursorScale()) < 0.01) {
      cur = i;
    }
  }
  cursorCombo_->setCurrentIndex(cur);
  connect(cursorCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, [this](int idx) {
            VideoWidget::setCursorScale(cursorCombo_->itemData(idx).toDouble());
          });
  lay->addWidget(cursorLabel);
  lay->addWidget(cursorCombo_);
  // 画质档位（0=自动 1=超清 2=高清 3=流畅）：切换经主窗口转发给会话，
  // 会话不存档位，重连后由主窗口用 currentQualityMode() 重放。
  qualityLabel_ = new QLabel(tr("画质"), toolbar_);
  qualityCombo_ = new QComboBox(toolbar_);
  qualityCombo_->addItem(tr("自动"), 0);
  qualityCombo_->addItem(tr("超清"), 1);
  qualityCombo_->addItem(tr("高清"), 2);
  qualityCombo_->addItem(tr("流畅"), 3);
  connect(qualityCombo_, QOverload<int>::of(&QComboBox::activated), this,
          [this](int) { emit qualityModeSelected(currentQualityMode()); });
  lay->addWidget(qualityLabel_);
  lay->addWidget(qualityCombo_);
  // 被控端屏幕（仅直连主控端显示）：切换后由主窗口决定如何生效（换屏重连）。
  screenLabel_ = new QLabel(tr("屏幕"), toolbar_);
  screenCombo_ = new QComboBox(toolbar_);
  screenCombo_->addItem(tr("被控端默认（连接后自动获取列表）"), 0);
  connect(screenCombo_, QOverload<int>::of(&QComboBox::activated), this,
          [this](int) { emit screenSelected(currentScreenId()); });
  lay->addWidget(screenLabel_);
  lay->addWidget(screenCombo_);
  fullscreenBtn_ = new QPushButton(tr("退出全屏"), toolbar_);
  fullscreenBtn_->setObjectName("fullscreenBtn");
  disconnectBtn_ = new QPushButton(tr("断开连接"), toolbar_);
  disconnectBtn_->setObjectName("disconnectBtn");
  lay->addWidget(fullscreenBtn_);
  lay->addWidget(disconnectBtn_);
  toolbar_->adjustSize();
  toolbar_->installEventFilter(this);

  connect(fullscreenBtn_, &QPushButton::clicked, this,
          &RemoteVideoWindow::toggleFullscreen);
  connect(disconnectBtn_, &QPushButton::clicked, this, [this] {
    emit disconnectRequested();
    // 断开连接后窗口自己关闭（会话收尾由主窗口的 disconnectRequested 处理）。
    close();
  });
}

void RemoteVideoWindow::setFrame(const QImage& image) {
  video_->setRemoteGeometry(image.width(), image.height());
  video_->setFrame(image);
}

void RemoteVideoWindow::setDiagText(const QString& text) {
  video_->setDiagText(text);
}

int RemoteVideoWindow::currentQualityMode() const {
  return qualityCombo_ ? qualityCombo_->currentData().toInt() : 0;
}

qint64 RemoteVideoWindow::currentScreenId() const {
  return screenCombo_ ? screenCombo_->currentData().toLongLong() : 0;
}

QString RemoteVideoWindow::screenTitle() const {
  return screenCombo_ ? screenCombo_->currentText()
                      : tr("被控端默认");
}

void RemoteVideoWindow::setScreens(const QVector<qint64>& ids,
                                   const QStringList& titles) {
  if (!screenCombo_) {
    return;
  }
  screenCombo_->blockSignals(true);
  const QString prev = screenCombo_->currentText();
  screenCombo_->clear();
  for (int i = 0; i < ids.size(); ++i) {
    QString name = i < titles.size() ? titles[i] : QString();
    if (name.isEmpty()) {
      name = tr("屏幕 %1").arg(i + 1);
    }
    screenCombo_->addItem(name, QVariant::fromValue<qint64>(ids[i]));
  }
  int idx = screenCombo_->findText(prev);
  if (idx < 0) {
    idx = 0;
  }
  screenCombo_->setCurrentIndex(idx);
  screenCombo_->setEnabled(true);
  screenCombo_->blockSignals(false);
}

void RemoteVideoWindow::setScreenSelectorVisible(bool visible) {
  if (screenLabel_) {
    screenLabel_->setVisible(visible);
  }
  if (screenCombo_) {
    screenCombo_->setVisible(visible);
  }
}

void RemoteVideoWindow::showFullscreen() {
  showFullScreen();
  fullscreenBtn_->setText(tr("退出全屏"));
  showToolbar();
}

void RemoteVideoWindow::toggleFullscreen() {
  if (isFullScreen()) {
    showNormal();
    fullscreenBtn_->setText(tr("全屏"));
  } else {
    showFullScreen();
    fullscreenBtn_->setText(tr("退出全屏"));
  }
  showToolbar();
}

void RemoteVideoWindow::showToolbar() {
  positionToolbar();
  toolbar_->show();
  toolbar_->raise();
  hideTimer_->start(kToolbarHideMs);
}

void RemoteVideoWindow::positionToolbar() {
  toolbar_->adjustSize();
  toolbar_->move((width() - toolbar_->width()) / 2, 14);
  static std::atomic<int> dbg{0};
  if (dbg.fetch_add(1) < 5) {
    rdDebug("toolbar", "window=%dx%d dpr=%.2f toolbar=%dx%d -> x=%d",
            width(), height(), devicePixelRatioF(), toolbar_->width(),
            toolbar_->height(), toolbar_->x());
  }
}

bool RemoteVideoWindow::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::MouseMove) {
    if (watched == video_) {
      // Near the top edge: reveal the toolbar and restart the hide timer.
      const QPoint p =
          video_->mapTo(this, static_cast<QMouseEvent*>(event)->pos());
      if (p.y() < kTopHotZone) {
        showToolbar();
      }
    } else if (watched == toolbar_) {
      // Cursor is on the toolbar: keep it visible while it moves there.
      hideTimer_->start(kToolbarHideMs);
    }
  }
  return QWidget::eventFilter(watched, event);
}

void RemoteVideoWindow::changeEvent(QEvent* event) {
  if (event->type() == QEvent::WindowStateChange) {
    const bool minimized = windowState() & Qt::WindowMinimized;
    // The trail matters: a user report of "frozen after minimize" must be
    // correlatable with the pipeline counters in this same file.
    rdDebug("window", "video window %s (frame geometry %dx%d)", minimized
                                                                 ? "minimized"
                                                                 : "restored",
            video_->remoteWidth(), video_->remoteHeight());
    if (!minimized) {
      // While minimized no paint events are delivered; force one fresh paint
      // on restore so the newest frame shows immediately even if the platform
      // coalesced the pending damage away.
      video_->update();
    }
  }
  QWidget::changeEvent(event);
}

void RemoteVideoWindow::resizeEvent(QResizeEvent*) {
  positionToolbar();
}

void RemoteVideoWindow::closeEvent(QCloseEvent* event) {
  // Closing the popup only hides it (the session keeps running); disconnect
  // happens via the toolbar button. Same-machine testing needs the popup out
  // of the way without killing the stream — it mirrors the captured screen.
  event->accept();
  hide();
}

}  // namespace rd
