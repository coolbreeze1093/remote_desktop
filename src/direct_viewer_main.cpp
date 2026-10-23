// rd_direct_viewer — 直连主控端入口：填 IP:端口直连被控端。
// 用法：rd_direct_viewer.exe [host] [端口]（缺省端口 48010；给参数则启动后自动连接）。
#include <QApplication>
#include <QStringList>

#include "ui/direct_viewer_window.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("RemoteDesktopDirectViewer"));
  rd::DirectViewerWindow win;
  win.show();
  const QStringList args = app.arguments();
  if (args.size() > 1) {
    bool ok = false;
    const quint16 port =
        (args.size() > 2) ? args.at(2).toUShort(&ok) : 48010;
    if (ok && port > 0) {
      win.autoConnect(args.at(1), port);
    }
  }
  return app.exec();
}
