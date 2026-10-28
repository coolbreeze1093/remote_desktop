#include "core/input_desktop.h"

#include <windows.h>

#include <atomic>

#include "debug_log.h"

namespace rd {

namespace {

// 与 vendored webrtc Desktop::GetInputDesktop 相同的访问掩码（GDI 采集与
// SendInput 所需的最小集合）。
constexpr DWORD kDesktopAccess =
    GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE;

// 线程本地已附着的输入桌面。h 为打开句柄，线程退出时尝试关闭；若线程仍
// 附着于该桌面，CloseDesktop 会失败，但线程终止时系统随句柄表回收，不泄漏。
struct ThreadDesktopState {
  HDESK h = nullptr;
  std::wstring name;
  ~ThreadDesktopState() {
    if (h) {
      CloseDesktop(h);
    }
  }
};

ThreadDesktopState& GetThreadDesktopState() {
  thread_local ThreadDesktopState state;
  return state;
}

std::wstring DesktopName(HDESK h) {
  DWORD needed = 0;
  if (!GetUserObjectInformationW(h, UOI_NAME, nullptr, 0, &needed) &&
      GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    return {};
  }
  std::wstring name(needed / sizeof(wchar_t), L'\0');
  if (!GetUserObjectInformationW(h, UOI_NAME, name.data(),
                                 static_cast<DWORD>(name.size() * sizeof(wchar_t)),
                                 &needed)) {
    return {};
  }
  while (!name.empty() && name.back() == L'\0') {
    name.pop_back();
  }
  return name;
}

}  // namespace

std::wstring CurrentInputDesktopName(bool* deniedOut, unsigned long* errOut) {
  if (deniedOut) {
    *deniedOut = false;
  }
  if (errOut) {
    *errOut = 0;
  }
  HDESK h = OpenInputDesktop(0, FALSE, kDesktopAccess);
  if (!h) {
    const DWORD err = GetLastError();
    if (deniedOut && err == ERROR_ACCESS_DENIED) {
      *deniedOut = true;
    }
    if (errOut) {
      *errOut = err;
    }
    return {};
  }
  const std::wstring name = DesktopName(h);
  CloseDesktop(h);
  return name;
}

bool IsSecureDesktopName(const std::wstring& name) {
  return !name.empty() && name != L"Default";
}

AttachResult AttachThisThreadToInputDesktop(std::wstring* nameOut) {
  ThreadDesktopState& st = GetThreadDesktopState();

  HDESK input = OpenInputDesktop(0, FALSE, kDesktopAccess);
  if (!input) {
    const DWORD err = GetLastError();
    // 打不开多因权限（未提权遇安全桌面 err=5）；留痕便于 rd_debug.log 定位。
    static std::atomic<int> openDbg{0};
    if (openDbg.fetch_add(1) < 10) {
      rdDebug("desktop", "OpenInputDesktop failed err=%lu", (unsigned long)err);
    }
    return err == ERROR_ACCESS_DENIED ? AttachResult::kOpenDenied
                                      : AttachResult::kOpenFailed;
  }
  const std::wstring name = DesktopName(input);
  if (name.empty()) {
    CloseDesktop(input);
    return AttachResult::kProbeFailed;
  }
  if (nameOut) {
    *nameOut = name;
  }
  if (st.h && st.name == name) {
    CloseDesktop(input);  // 已附着在同一桌面，多余的句柄关掉
    return AttachResult::kOnInputDesktop;
  }
  // SetThreadDesktop 失败时线程仍留在原桌面（desktop 对象随 CloseDesktop 回收）。
  if (!SetThreadDesktop(input)) {
    CloseDesktop(input);
    return AttachResult::kSetFailed;
  }
  if (st.h) {
    CloseDesktop(st.h);  // 已切到新桌面，旧句柄可关
  }
  st.h = input;
  st.name = name;
  return AttachResult::kOnInputDesktop;
}

bool IsProcessElevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return false;
  }
  DWORD elevated = 0;
  DWORD returned = 0;
  const BOOL ok = GetTokenInformation(token, TokenElevation, &elevated,
                                      sizeof(elevated), &returned);
  CloseHandle(token);
  return ok && elevated;
}

bool CanOpenSecureDesktop() {
  // 以令牌提权状态为准（TokenElevation，SYSTEM 亦为真）；名字探测仅在
  // 非提权进程意外可开时作为附加肯定信号，绝不因它单独失败而误报。
  if (IsProcessElevated()) {
    return true;
  }
  HDESK h = OpenDesktopW(L"Winlogon", 0, FALSE, kDesktopAccess);
  if (!h) {
    return false;
  }
  CloseDesktop(h);
  return true;
}

}  // namespace rd
