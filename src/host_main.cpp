// rd_host — 远程桌面服务端（被控端）GUI 入口。
// 本机推流 + 被控服务面板；锁屏/UAC 的 SYSTEM 级采集与控制在
// rd_host_service（见 service/）里，本程序只负责界面与手动 SDP 交换。
#include <windows.h>

#include <QApplication>

#include "core/input_desktop.h"
#include "debug_log.h"
#include "ui/host_window.h"

int main(int argc, char** argv) {
  // 提权状态只落日志：锁屏/UAC 的 SYSTEM 级采集与控制已移交被控服务
  // （rd_host_service），GUI 不再启动时自提权——普通双击即可用。
  rdDebug("perm", "host startup elevated=%d", (int)rd::IsProcessElevated());
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("RemoteDesktopHost"));
  rd::HostWindow win;
  win.show();
  return app.exec();
}
