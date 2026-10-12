#include "core/input_desktop.h"

#include <windows.h>

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

std::wstring CurrentInputDesktopName() {
  HDESK h = OpenInputDesktop(0, FALSE, kDesktopAccess);
  if (!h) {
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
    return GetLastError() == ERROR_ACCESS_DENIED ? AttachResult::kOpenDenied
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

bool CanOpenSecureDesktop() {
  HDESK h = OpenDesktopW(L"Winlogon", 0, FALSE, kDesktopAccess);
  if (!h) {
    return false;
  }
  CloseDesktop(h);
  return true;
}

}  // namespace rd
