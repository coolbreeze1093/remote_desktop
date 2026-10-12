// rdc — 引擎与宿主共用的诊断日志（header-only）。追加写 exe 同目录
// rd_debug.log；GUI 启动没有 stderr，这份文件就是"连上了但没画面"类问题
// 的诊断线索。无 Qt 依赖（GetModuleFileNameW 定位 exe 目录）。
#ifndef RDC_LOG_H_
#define RDC_LOG_H_

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

inline void rdcDebug(const char* tag, const char* fmt, ...) {
  static std::atomic<bool> disabled{false};
  if (disabled.load(std::memory_order_relaxed)) {
    return;
  }
#ifdef _WIN32
  static const std::wstring logPath = [] {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring ws = (n > 0) ? std::wstring(buf, n) : std::wstring(L"rd_debug.log");
    const size_t slash = ws.find_last_of(L"\\/");
    return (slash == std::wstring::npos)
               ? std::wstring(L"rd_debug.log")
               : ws.substr(0, slash + 1) + L"rd_debug.log";
  }();
  FILE* f = _wfopen(logPath.c_str(), L"a");
#else
  FILE* f = std::fopen("rd_debug.log", "a");
#endif
  if (!f) {
    disabled.store(true, std::memory_order_relaxed);
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

#endif  // RDC_LOG_H_
