// rd_viewer — 远程桌面客户端（主控端）GUI 入口。
// 创建 Offer、粘贴 Answer 建连，显示远端画面并转发键鼠输入。
#include <QApplication>

#include "ui/viewer_window.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("RemoteDesktopViewer"));
  rd::ViewerWindow win;
  win.show();
  return app.exec();
}
