// Vendored compatibility shim for <comdef.h>.
// MSVC: forwards to the real header. Other compilers (older mingw-w64 where
// <comdef.h> fails to build): provides the minimal _com_error/_bstr_t surface
// used by the WebRTC desktop_capture sources.
#ifndef SCL_COMPAT_COMDEF_SHIM_H_
#define SCL_COMPAT_COMDEF_SHIM_H_

#if defined(_MSC_VER)
#include <comdef.h>
#else
#include <windows.h>
#include <string>

namespace scrl_shim {
inline std::string WideToUtf8(const wchar_t* w) {
  if (!w) return {};
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n - 1, nullptr, nullptr);
  return s;
}
}  // namespace scrl_shim

class _bstr_t {
 public:
  explicit _bstr_t(const wchar_t* s) : str_(scrl_shim::WideToUtf8(s)) {}
  const char* c_str() const { return str_.c_str(); }

 private:
  std::string str_;
};

class _com_error {
 public:
  _com_error(HRESULT hr) : hr_(hr) {
    LPWSTR msg = nullptr;
    if (::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                             FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS,
                         nullptr, hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                         reinterpret_cast<LPWSTR>(&msg), 0, nullptr) &&
        msg) {
      message_ = scrl_shim::WideToUtf8(msg);
      ::LocalFree(msg);
    }
  }
  HRESULT Error() const { return hr_; }
  const wchar_t* ErrorMessage() const { return message_.empty() ? nullptr : nullptr; }
  const std::string& message() const { return message_; }

 private:
  HRESULT hr_;
  std::string message_;
};
#endif  // _MSC_VER

#endif  // SCL_COMPAT_COMDEF_SHIM_H_
