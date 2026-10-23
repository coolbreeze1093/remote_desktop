// rd_direct_host — 直连被控端入口：监听端口，主控端连上即自动建远程。
// 用法：rd_direct_host.exe [端口]（缺省 48010；给端口则启动后自动监听）。
// 推流优先走被控服务 rd_host_service（与 rd_host 一样先在窗口里
// 「安装被控服务」；SYSTEM 身份，锁屏/UAC/登录界面均可看可控），
// 服务未装时回退本机模式（锁屏/UAC 不可控）。
#include <windows.h>

#include <QApplication>
#include <QStringList>

#include "core/input_desktop.h"
#include "debug_log.h"
#include "ui/direct_host_window.h"

int main(int argc, char** argv) {
  rdDebug("perm", "direct-host startup elevated=%d", (int)rd::IsProcessElevated());
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("RemoteDesktopDirectHost"));
  rd::DirectHostWindow win;
  win.show();
  const QStringList args = app.arguments();
  if (args.size() > 1) {
    bool ok = false;
    const quint16 port = args.at(1).toUShort(&ok);
    if (ok && port > 0) {
      win.autoListen(port);
    }
  }
  return app.exec();
}
