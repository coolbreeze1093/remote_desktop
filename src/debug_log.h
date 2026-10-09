// remote_desktop — always-on trace helper (low-volume call sites only).
// Appends to rd_debug.log next to the exe; the file is the diagnostic trail
// for "connected but no picture" reports since GUI launches have no stderr.
#ifndef REMOTE_DESKTOP_DEBUG_LOG_H_
#define REMOTE_DESKTOP_DEBUG_LOG_H_

#include <atomic>
#include <cstdio>
#include <cstdarg>

#include <QCoreApplication>
#include <QString>

inline void rdDebug(const char* tag, const char* fmt, ...) {
  static const QString logPath = [] {
    const QString dir = QCoreApplication::applicationDirPath();
    return dir.isEmpty() ? QStringLiteral("rd_debug.log")
                         : dir + QStringLiteral("/rd_debug.log");
  }();
  static std::atomic<bool> disabled{false};
  if (disabled.load()) {
    return;
  }
  FILE* f = std::fopen(QByteArray(logPath.toLocal8Bit()).constData(), "a");
  if (!f) {
    disabled.store(true);
    return;
  }
  std::fprintf(f, "[%s] ", tag);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(f, fmt, ap);
  va_end(ap);
  std::fprintf(f, "\n");
  std::fclose(f);
}

#endif
