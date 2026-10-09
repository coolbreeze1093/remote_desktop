#include <QApplication>

#include "ui/main_window.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("RemoteDesktop"));
  rd::MainWindow win;
  win.show();
  return app.exec();
}
