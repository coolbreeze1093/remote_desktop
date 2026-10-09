#!/usr/bin/env python3
"""Vendor the WebRTC desktop-capture source subset into screen_capture_lib/vendored.

Read-only against the WebRTC tree: every source file is *copied*, never moved
or modified. The WebRTC tree must stay untouched except for screen_capture_lib/.

The script seeds the file set from the research lists plus the per-platform
source lists extracted from modules/desktop_capture/BUILD.gn, then traces
quoted #include directives transitively and copies the full closure.

Platform coverage:
  - Windows : always vendored (GDI / DXGI / WGC)
  - Linux   : X11 files + *_linux.cc entry points (+ optional PipeWire/portal)
  - macOS   : mac/ objc+c++ files + *_mac.mm entry points + sdk/objc helpers

Usage:
  python tools/vendor_from_webrtc.py --src E:\\Github\\newwebrtc\\webrtc\\src
"""
import argparse
import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LIB_ROOT = os.path.dirname(HERE)
RESEARCH = os.path.join(LIB_ROOT, "_research")

CLOSURE_LIST = os.path.join(RESEARCH, "closure_webrtc_vendored.txt")
CANDIDATE_LIST = os.path.join(RESEARCH, "candidate_runtime_symbols.txt")

# ---------------------------------------------------------------------------
# Runtime-symbol sources not covered by the closure list (needed at link time).
# ---------------------------------------------------------------------------
RUNTIME_SOURCES = [
    "system_wrappers/source/metrics.cc",
    "system_wrappers/source/cpu_features.cc",
    "system_wrappers/source/sleep.cc",
    "rtc_base/platform_thread.cc",
    "system_wrappers/include/metrics.h",
    "system_wrappers/include/cpu_features_wrapper.h",
]

# ---------------------------------------------------------------------------
# Linux / X11: modules/desktop_capture/BUILD.gn "rtc_use_x11_extensions" block
# plus the WEBRTC_LINUX entry points (screen_capturer_linux.cc etc.).
# ---------------------------------------------------------------------------
LINUX_X11_SOURCES = [
    "modules/desktop_capture/mouse_cursor_monitor_linux.cc",
    "modules/desktop_capture/screen_capturer_linux.cc",
    "modules/desktop_capture/window_capturer_linux.cc",
    "modules/desktop_capture/linux/x11/mouse_cursor_monitor_x11.cc",
    "modules/desktop_capture/linux/x11/mouse_cursor_monitor_x11.h",
    "modules/desktop_capture/linux/x11/screen_capturer_x11.cc",
    "modules/desktop_capture/linux/x11/screen_capturer_x11.h",
    "modules/desktop_capture/linux/x11/shared_x_display.cc",
    "modules/desktop_capture/linux/x11/shared_x_display.h",
    "modules/desktop_capture/linux/x11/window_capturer_x11.cc",
    "modules/desktop_capture/linux/x11/window_capturer_x11.h",
    "modules/desktop_capture/linux/x11/window_finder_x11.cc",
    "modules/desktop_capture/linux/x11/window_finder_x11.h",
    "modules/desktop_capture/linux/x11/window_list_utils.cc",
    "modules/desktop_capture/linux/x11/window_list_utils.h",
    "modules/desktop_capture/linux/x11/x_atom_cache.cc",
    "modules/desktop_capture/linux/x11/x_atom_cache.h",
    "modules/desktop_capture/linux/x11/x_error_trap.cc",
    "modules/desktop_capture/linux/x11/x_error_trap.h",
    "modules/desktop_capture/linux/x11/x_server_pixel_buffer.cc",
    "modules/desktop_capture/linux/x11/x_server_pixel_buffer.h",
    "modules/desktop_capture/linux/x11/x_window_property.cc",
    "modules/desktop_capture/linux/x11/x_window_property.h",
]

# ---------------------------------------------------------------------------
# Linux / Wayland (PipeWire via XDG desktop portal). Optional: heavy deps
# (glib, gbm, egl, epoxy, libdrm, libpipewire-0.3) resolved via pkg-config.
# ---------------------------------------------------------------------------
LINUX_PIPEWIRE_SOURCES = [
    "modules/desktop_capture/linux/wayland/base_capturer_pipewire.cc",
    "modules/desktop_capture/linux/wayland/base_capturer_pipewire.h",
    "modules/desktop_capture/linux/wayland/egl_dmabuf.cc",
    "modules/desktop_capture/linux/wayland/egl_dmabuf.h",
    "modules/desktop_capture/linux/wayland/mouse_cursor_monitor_pipewire.cc",
    "modules/desktop_capture/linux/wayland/mouse_cursor_monitor_pipewire.h",
    "modules/desktop_capture/linux/wayland/restore_token_manager.cc",
    "modules/desktop_capture/linux/wayland/restore_token_manager.h",
    "modules/desktop_capture/linux/wayland/screen_capture_portal_interface.cc",
    "modules/desktop_capture/linux/wayland/screen_capture_portal_interface.h",
    "modules/desktop_capture/linux/wayland/screencast_portal.cc",
    "modules/desktop_capture/linux/wayland/screencast_portal.h",
    "modules/desktop_capture/linux/wayland/screencast_stream_utils.cc",
    "modules/desktop_capture/linux/wayland/screencast_stream_utils.h",
    "modules/desktop_capture/linux/wayland/shared_screencast_stream.cc",
    "modules/desktop_capture/linux/wayland/shared_screencast_stream.h",
    "modules/desktop_capture/linux/wayland/xdg_desktop_portal_utils.cc",
    "modules/desktop_capture/linux/wayland/xdg_desktop_portal_utils.h",
    "modules/desktop_capture/linux/wayland/xdg_session_details.h",
    "modules/portal/pipewire_utils.cc",
    "modules/portal/pipewire_utils.h",
    "modules/portal/portal_request_response.h",
    "modules/portal/scoped_glib.cc",
    "modules/portal/scoped_glib.h",
    "modules/portal/xdg_desktop_portal_utils.cc",
    "modules/portal/xdg_desktop_portal_utils.h",
    "modules/portal/xdg_session_details.h",
    "modules/portal/pipewire.sigs",
    "modules/portal/pipewire_stub_header.fragment",
]

# macOS: desktop_capture target's is_mac block + desktop_capture_objc target
# + the module-root *_mac.mm entry points. screen_capturer_darwin.mm is
# listed by BUILD.gn but absent in this tree; missing files are skipped.
MACOS_SOURCES = [
    "modules/desktop_capture/mouse_cursor_monitor_mac.mm",
    "modules/desktop_capture/window_capturer_mac.mm",
    "modules/desktop_capture/window_finder_mac.mm",
    "modules/desktop_capture/mac/desktop_configuration.h",
    "modules/desktop_capture/mac/desktop_configuration.mm",
    "modules/desktop_capture/mac/desktop_configuration_monitor.cc",
    "modules/desktop_capture/mac/desktop_configuration_monitor.h",
    "modules/desktop_capture/mac/desktop_frame_cgimage.h",
    "modules/desktop_capture/mac/desktop_frame_cgimage.mm",
    "modules/desktop_capture/mac/desktop_frame_iosurface.h",
    "modules/desktop_capture/mac/desktop_frame_iosurface.mm",
    "modules/desktop_capture/mac/desktop_frame_provider.h",
    "modules/desktop_capture/mac/desktop_frame_provider.mm",
    "modules/desktop_capture/mac/desktop_frame_utils.cc",
    "modules/desktop_capture/mac/desktop_frame_utils.h",
    "modules/desktop_capture/mac/full_screen_mac_application_handler.cc",
    "modules/desktop_capture/mac/full_screen_mac_application_handler.h",
    "modules/desktop_capture/mac/screen_capturer_mac.h",
    "modules/desktop_capture/mac/screen_capturer_mac.mm",
    "modules/desktop_capture/mac/window_list_utils.cc",
    "modules/desktop_capture/mac/window_list_utils.h",
    "modules/desktop_capture/desktop_capture_metadata.h",
]

# Objective-C helpers used by the mac capture files (from sdk:helpers_objc;
# trimmed to what desktop_capture actually pulls in).
MACOS_SDK_OBJC_SOURCES = [
    "sdk/objc/helpers/NSString+StdString.h",
    "sdk/objc/helpers/NSString+StdString.mm",
    "sdk/objc/helpers/RTCDispatcher+Private.h",
    "sdk/objc/helpers/RTCDispatcher.h",
    "sdk/objc/helpers/RTCDispatcher.m",
    "sdk/objc/helpers/RTCYUVHelper.h",
    "sdk/objc/helpers/RTCYUVHelper.mm",
    "sdk/objc/helpers/scoped_cftyperef.h",
    "sdk/objc/base/RTCLogging.h",
    "sdk/objc/base/RTCLogging.mm",
]

# ---------------------------------------------------------------------------
# Abseil translation units compiled into the lib (rest is header-only).
# ---------------------------------------------------------------------------
ABSL_CC_CANDIDATES = [
    "third_party/abseil-cpp/absl/types/bad_optional_access.cc",
    "third_party/abseil-cpp/absl/strings/ascii.cc",
    "third_party/abseil-cpp/absl/strings/match.cc",
    "third_party/abseil-cpp/absl/strings/internal/memutil.cc",
    "third_party/abseil-cpp/absl/base/internal/raw_logging.cc",
    "third_party/abseil-cpp/absl/base/internal/throw_delegate.cc",
    "third_party/abseil-cpp/absl/numeric/int128.cc",
]

LIBYUV_EXCLUDE_RE = re.compile(
    r"(_neon\.cc|_neon64\.cc|_msa\.cc|_lsx\.cc|_lasx\.cc|"
    r"_rvv\.cc|jpeg.*\.cc|mjpeg.*\.cc)$"
)
LIBYUV_MSVCC_RE = re.compile(r"_win\.cc$")
LIBYUV_GNU_RE = re.compile(r"_gcc\.cc$")

LICENSE_FILES = [
    ("LICENSE", "LICENSE.webrtc"),
    ("third_party/libyuv/LICENSE", "third_party/libyuv/LICENSE"),
    ("third_party/libyuv/PATENTS", "third_party/libyuv/PATENTS"),
    ("third_party/abseil-cpp/LICENSE", "third_party/abseil-cpp/LICENSE"),
]

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)

# Post-vendor source patches for compilers other than MSVC. Each patch is
# (file, old_text, new_text) and is applied only if old_text is present.
# These keep the vendored copy compilable with MinGW/GCC without touching
# the upstream WebRTC tree.
PATCHES = [
    (
        "rtc_base/platform_thread_types.cc",
        """  __try {
    ::RaiseException(0x406D1388, 0, sizeof(threadname_info) / sizeof(ULONG_PTR),
                     reinterpret_cast<ULONG_PTR*>(&threadname_info));
  } __except (EXCEPTION_EXECUTE_HANDLER) {  // NOLINT
  }""",
        """#if defined(_MSC_VER)
  __try {
    ::RaiseException(0x406D1388, 0, sizeof(threadname_info) / sizeof(ULONG_PTR),
                     reinterpret_cast<ULONG_PTR*>(&threadname_info));
  } __except (EXCEPTION_EXECUTE_HANDLER) {  // NOLINT
  }
#endif  // defined(_MSC_VER)""",
    ),
    (
        "rtc_base/win/windows_version.cc",
        "#if !defined(__clang__) && _MSC_FULL_VER < 191125507",
        "#if defined(_MSC_VER) && !defined(__clang__) && _MSC_FULL_VER < 191125507",
    ),
    (
        # Old mingw-w64 winuser.h lacks this Win8.1 PrintWindow flag.
        "modules/desktop_capture/win/window_capturer_win_gdi.cc",
        "const UINT flags = PW_RENDERFULLCONTENT;",
        "const UINT flags = 0x2;  // PW_RENDERFULLCONTENT (absent in old MinGW headers)",
    ),
    (
        # Old mingw-w64 lacks <shellscalingapi.h>; resolve GetDpiForMonitor
        # at runtime from shcore.dll so no shcore import library is needed
        # (Qt's MinGW 7.3 does not ship libshcore.a).
        "modules/desktop_capture/win/screen_capture_utils.cc",
        "#include <shellscalingapi.h>",
        """#if defined(__MINGW32__) && !defined(MDT_EFFECTIVE_DPI)
// MinGW shim for <shellscalingapi.h>: only what this TU needs. Resolved at
// runtime from shcore.dll so that no shcore import library is required.
#include <windows.h>
typedef enum MONITOR_DPI_TYPE {
  MDT_EFFECTIVE_DPI = 0,
  MDT_ANGULAR_DPI = 1,
  MDT_RAW_DPI = 2
} MONITOR_DPI_TYPE;
namespace {
using GetDpiForMonitorFunc = HRESULT(__stdcall*)(HMONITOR, MONITOR_DPI_TYPE,
                                                 UINT*, UINT*);
GetDpiForMonitorFunc LoadGetDpiForMonitor() {
  static GetDpiForMonitorFunc const func = [] {
    HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
    return shcore ? reinterpret_cast<GetDpiForMonitorFunc>(
                        ::GetProcAddress(shcore, "GetDpiForMonitor"))
                  : nullptr;
  }();
  return func;
}
}  // namespace
HRESULT GetDpiForMonitorShim(HMONITOR hmonitor, MONITOR_DPI_TYPE dpiType,
                             UINT* dpiX, UINT* dpiY) {
  GetDpiForMonitorFunc func = LoadGetDpiForMonitor();
  if (!func) return E_NOTIMPL;
  return func(hmonitor, dpiType, dpiX, dpiY);
}
#else
#include <shellscalingapi.h>
#endif""",
    ),
    (
        "modules/desktop_capture/win/screen_capture_utils.cc",
        "HRESULT hr = ::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);",
        """#if defined(__MINGW32__) && !defined(MDT_EFFECTIVE_DPI)
  HRESULT hr = GetDpiForMonitorShim(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
#else
  HRESULT hr = ::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
#endif""",
    ),
    (
        # MinGW-w64 (GCC 7.3 era) headers lack IVirtualDesktopManager; declare
        # it with the same guard macro the Windows SDK uses, so MSVC builds
        # (where the SDK already declares it) skip this block.
        "modules/desktop_capture/win/window_capture_utils.h",
        "namespace webrtc {",
        """// Vendored for MinGW: the Windows SDK declares IVirtualDesktopManager in
// shobjidl_core.h; older mingw-w64 headers do not. Same guard macro as the SDK.
#ifndef __IVirtualDesktopManager_INTERFACE_DEFINED__
#define __IVirtualDesktopManager_INTERFACE_DEFINED__
class IVirtualDesktopManager : public IUnknown {
 public:
  virtual HRESULT STDMETHODCALLTYPE IsWindowOnCurrentVirtualDesktop(
      HWND topLevelWindow, BOOL* onDesktop) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetWindowDesktopId(HWND topLevelWindow,
                                                        GUID* desktopId) = 0;
  virtual HRESULT STDMETHODCALLTYPE MoveWindowToDesktop(HWND topLevelWindow,
                                                         REFGUID desktopId) = 0;
};
#endif

namespace webrtc {""",
    ),
    (
        "modules/desktop_capture/win/window_capture_utils.cc",
        """    if (FAILED(::CoCreateInstance(__uuidof(VirtualDesktopManager), nullptr,
                                  CLSCTX_ALL,
                                  IID_PPV_ARGS(&virtual_desktop_manager_)))) {""",
        """    // Vendored for MinGW: explicit CLSID/IID instead of __uuidof.
    static const CLSID kCLSID_VirtualDesktopManager = {
        0xaa509086, 0x5ca9, 0x4c25,
        {0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb0, 0xaa}};
    static const IID kIID_IVirtualDesktopManager = {
        0xa5cd92ff, 0x29be, 0x454c,
        {0x8d, 0x04, 0xd8, 0x28, 0x79, 0xfb, 0x3f, 0x1b}};
    if (FAILED(::CoCreateInstance(
            kCLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL,
            kIID_IVirtualDesktopManager,
            reinterpret_cast<void**>(virtual_desktop_manager_.GetAddressOf())))) {""",
    ),
    (
        # Thin marker patch: the actual <comdef.h> -> shim rewrite happens
        # globally over all copied files in main() (many TUs include it).
        "modules/desktop_capture/win/desktop_capture_utils.h",
        "#include <comdef.h>",
        "#include \"scl_compat/comdef_shim.h\"",
    ),
    (
        "modules/desktop_capture/win/desktop_capture_utils.cc",
        """std::string ComErrorToString(const _com_error& error) {
  char buffer[1024];
  rtc::SimpleStringBuilder string_builder(buffer);
  // Use _bstr_t to simplify the wchar to char conversion for ErrorMessage().
  _bstr_t error_message(error.ErrorMessage());
  string_builder.AppendFormat("HRESULT: 0x%08X, Message: %s", error.Error(),
                              static_cast<const char*>(error_message));
  return string_builder.str();""",
        """std::string ComErrorToString(const _com_error& error) {
  char buffer[1024];
  rtc::SimpleStringBuilder string_builder(buffer);
#if defined(_MSC_VER)
  // Use _bstr_t to simplify the wchar to char conversion for ErrorMessage().
  _bstr_t error_message(error.ErrorMessage());
  string_builder.AppendFormat("HRESULT: 0x%08X, Message: %s", error.Error(),
                              static_cast<const char*>(error_message));
#else
  string_builder.AppendFormat("HRESULT: 0x%08X, Message: %s", error.Error(),
                              error.message().c_str());
#endif
  return string_builder.str();""",
    ),
]

# Generated compatibility header: older mingw-w64 <comdef.h> does not
# compile (swprintf_s not declared). Written to scl_compat/comdef_shim.h and
# every `#include <comdef.h>` in the vendored tree is redirected to it.
COMDEF_SHIM_H = r'''// Vendored compatibility shim for <comdef.h>.
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
'''

ABS_PREFIX = "E:/Github/newwebrtc/webrtc/src/"

# Include roots for quoted-include resolution.
INCLUDE_ROOTS = ["", "third_party/abseil-cpp", "third_party/libyuv/include"]


def norm(rel):
    return rel.replace("\\", "/").strip()


def read_lines(path, strip_prefix=True):
    out = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if strip_prefix and line.startswith(ABS_PREFIX):
                line = line[len(ABS_PREFIX):]
            out.append(norm(line))
    return out


def includes_of(text):
    return [m.group(1).replace("\\", "/") for m in INCLUDE_RE.finditer(text)]


def resolve(inc, src_root):
    """Resolve one quoted include against the include roots; None if absent."""
    if inc.startswith(("winrt/", "windows.", "Windows.")):
        return None  # SDK headers
    for root in INCLUDE_ROOTS:
        cand = norm(os.path.join(root, inc)) if root else norm(inc)
        if os.path.isfile(os.path.join(src_root, cand)):
            return cand
    return None


def trace_closure(seeds, src_root):
    """Transitive quoted-include closure over the seeded files."""
    seen = set()
    missing = []
    queue = [f for f in seeds if f.endswith((".cc", ".h", ".c", ".mm", ".m", ".frag"))]
    seen.update(queue)
    while queue:
        rel = queue.pop()
        path = os.path.join(src_root, norm(rel))
        if not os.path.isfile(path):
            continue
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        for inc in includes_of(text):
            cand = resolve(inc, src_root)
            if cand is None:
                if not inc.startswith("<"):
                    missing.append(inc)
                continue
            if cand not in seen:
                seen.add(cand)
                queue.append(cand)
    resolved = {f for f in seen
                if os.path.isfile(os.path.join(src_root, norm(f)))}
    return resolved, sorted(set(missing))


def classify_libyuv(src_root):
    """Split libyuv sources into generic / msvc(_win) / gnu(_gcc) groups."""
    src_dir = os.path.join(src_root, "third_party", "libyuv", "source")
    generic, msvc, gnu, skipped = [], [], [], []
    for name in sorted(os.listdir(src_dir)):
        if not name.endswith((".cc", ".c")):
            continue
        rel = "third_party/libyuv/source/" + name
        if LIBYUV_EXCLUDE_RE.search(name):
            skipped.append(name)
        elif LIBYUV_MSVCC_RE.search(name):
            msvc.append(rel)
        elif LIBYUV_GNU_RE.search(name):
            gnu.append(rel)
        else:
            generic.append(rel)
    return generic, msvc, gnu, skipped


def copy_file(src_root, rel, dst_root, copied, missing_ok=False):
    rel = norm(rel)
    if rel in copied:
        return True
    src = os.path.join(src_root, rel)
    dst = os.path.join(dst_root, rel)
    if not os.path.isfile(src):
        if missing_ok:
            return False
        raise RuntimeError("missing in webrtc tree: " + rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)
    copied.add(rel)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", default=os.path.join(LIB_ROOT, "vendored"))
    ap.add_argument("--no-pipewire", action="store_true",
                    help="skip Linux PipeWire/Wayland portal sources")
    args = ap.parse_args()
    src_root = os.path.abspath(args.src)
    if not os.path.isfile(os.path.join(src_root, "modules", "desktop_capture",
                                       "desktop_capturer.h")):
        raise RuntimeError("--src does not look like a webrtc checkout")
    out_root = os.path.abspath(args.out)

    if os.path.isdir(out_root):
        shutil.rmtree(out_root)
    os.makedirs(out_root)

    closure = read_lines(CLOSURE_LIST)
    candidates = read_lines(CANDIDATE_LIST)
    cand_files = []
    for line in candidates:
        if " -> " in line:
            cand_files.extend(norm(p) for p in line.split(" -> "))
        else:
            cand_files.append(line)

    libyuv_generic, libyuv_msvc, libyuv_gnu, libyuv_skipped = classify_libyuv(src_root)

    seeds = (closure + cand_files + RUNTIME_SOURCES +
             LINUX_X11_SOURCES + MACOS_SOURCES + MACOS_SDK_OBJC_SOURCES +
             ABSL_CC_CANDIDATES + libyuv_generic + libyuv_msvc + libyuv_gnu)
    if not args.no_pipewire:
        seeds += LINUX_PIPEWIRE_SOURCES

    closure_files, missing = trace_closure(seeds, src_root)

    copied = set()
    # Explicit platform/seed files: tolerate per-tree gaps (this fork deleted
    # some files BUILD.gn still references, e.g. screen_capturer_darwin.mm).
    for rel in seeds:
        copy_file(src_root, rel, out_root, copied, missing_ok=True)
    # Closure-traced files: must all exist (resolver guarantees it).
    for rel in sorted(closure_files):
        copy_file(src_root, rel, out_root, copied)

    # Apply compiler-compat patches to the vendored copies.
    applied_patches = []
    comdef_count = 0
    for root, _dirs, files in os.walk(out_root):
        for name in files:
            if not name.endswith((".h", ".cc", ".c", ".mm")):
                continue
            path = os.path.join(root, name)
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                text = f.read()
            if "#include <comdef.h>" in text:
                text = text.replace("#include <comdef.h>",
                                    '#include "scl_compat/comdef_shim.h"')
                comdef_count += 1
                with open(path, "w", encoding="utf-8", newline="") as f:
                    f.write(text)
    shim_dir = os.path.join(out_root, "scl_compat")
    os.makedirs(shim_dir, exist_ok=True)
    with open(os.path.join(shim_dir, "comdef_shim.h"), "w", encoding="utf-8",
              newline="") as f:
        f.write(COMDEF_SHIM_H)
    if comdef_count:
        print("rewrote %d <comdef.h> includes to scl_compat/comdef_shim.h"
              % comdef_count)
    for rel, old, new in PATCHES:
        path = os.path.join(out_root, norm(rel))
        if not os.path.isfile(path):
            continue
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        if old in text:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(text.replace(old, new, 1))
            applied_patches.append(rel)
        else:
            print("NOTE: patch target not found in " + rel)

    # Whole libyuv include tree (tiny, keeps headers consistent).
    yuv_inc = os.path.join(src_root, "third_party", "libyuv", "include")
    for dirpath, _d, filenames in os.walk(yuv_inc):
        for name in filenames:
            full = os.path.join(dirpath, name)
            rel = "third_party/libyuv/include/" + os.path.relpath(full, yuv_inc)
            copy_file(src_root, rel.replace("\\", "/"), out_root, copied)

    for src_rel, dst_rel in LICENSE_FILES:
        src = os.path.join(src_root, norm(src_rel))
        dst = os.path.join(out_root, norm(dst_rel))
        if os.path.isfile(src):
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst)

    def pick(prefix, exts, platform_dir=None):
        return sorted(
            f for f in copied
            if f.startswith(prefix) and f.endswith(exts)
            and (platform_dir is None or platform_dir in f))

    desktop_capture_common = pick("modules/desktop_capture/", (".cc",)) if False else None

    # --- grouped source lists for CMake -------------------------------------
    dc = "modules/desktop_capture/"
    # Module-root files carry platform suffixes (_win.cc / _linux.cc / .mm);
    # everything else at the root is platform-neutral.
    common_dc = sorted(
        f for f in copied
        if f.startswith(dc) and f.endswith(".cc")
        and not f.endswith("_win.cc") and not f.endswith("_linux.cc")
        and "/win/" not in f and "/linux/" not in f and "/mac/" not in f)
    win_dc = sorted(
        f for f in copied if f.startswith(dc)
        and (f.endswith(".cc") and ("/win/" in f or f.endswith("_win.cc"))))
    x11_dc = sorted(
        f for f in copied if f.startswith(dc + "linux/x11/") and f.endswith(".cc"))
    linux_entries = sorted(
        f for f in copied if f.startswith(dc) and f.endswith("_linux.cc"))
    mac_cc = sorted(
        f for f in copied if f.startswith(dc + "mac/") and f.endswith(".cc"))
    mac_mm = sorted(
        f for f in copied if f.startswith(dc) and f.endswith(".mm"))
    pipewire_dc = sorted(
        f for f in copied if f.startswith(dc + "linux/wayland/") and f.endswith(".cc"))
    portal_cc = sorted(
        f for f in copied if f.startswith("modules/portal/") and f.endswith(".cc"))
    runtime_cc = sorted(
        f for f in copied
        if (f.startswith("rtc_base/") or f.startswith("api/")
            or f.startswith("system_wrappers/")) and f.endswith(".cc"))
    sdk_objc = sorted(
        f for f in copied if f.startswith("sdk/objc/")
        and f.endswith((".mm", ".m")))

    with open(os.path.join(out_root, "manifest.cmake"), "w", encoding="utf-8") as f:
        f.write("# Generated by tools/vendor_from_webrtc.py -- do not edit.\n")
        f.write("# Vendored from WebRTC BSD-licensed sources; see LICENSE.webrtc.\n\n")
        groups = (
            ("SCL_DC_COMMON_SOURCES", common_dc),
            ("SCL_DC_WIN_SOURCES", win_dc),
            ("SCL_DC_X11_SOURCES", x11_dc + linux_entries),
            ("SCL_DC_MAC_CC_SOURCES", mac_cc),
            ("SCL_DC_MAC_OBJC_SOURCES", [m for m in mac_mm if m.startswith(dc)]),
            ("SCL_DC_PIPEWIRE_SOURCES", pipewire_dc + portal_cc),
            ("SCL_RUNTIME_SOURCES", runtime_cc),
            ("SCL_SDK_OBJC_SOURCES", sdk_objc),
            ("SCL_ABSL_SOURCES", sorted(
                f for f in copied if f.startswith("third_party/abseil-cpp/")
                and f.endswith(".cc"))),
            ("SCL_LIBYUV_GENERIC_SOURCES", libyuv_generic),
            ("SCL_LIBYUV_MSVC_SOURCES", sorted(
                f for f in copied if f.startswith("third_party/libyuv/source/")
                and LIBYUV_MSVCC_RE.search(f))),
            ("SCL_LIBYUV_GNU_SOURCES", sorted(
                f for f in copied if f.startswith("third_party/libyuv/source/")
                and LIBYUV_GNU_RE.search(f))),
        )
        for name, items in groups:
            f.write("set(%s\n" % name)
            for rel in items:
                f.write("  ${SCL_VENDOR_DIR}/" + rel + "\n")
            f.write(")\n\n")

    summary = {
        "total_files": len(copied),
        "applied_patches": applied_patches,
        "common_desktop_capture": common_dc,
        "win_desktop_capture": win_dc,
        "x11_sources": x11_dc + linux_entries,
        "mac_sources": mac_cc + mac_mm,
        "sdk_objc_sources": sdk_objc,
        "pipewire_sources": pipewire_dc + portal_cc,
        "runtime_sources": runtime_cc,
        "absl_sources": sorted(
            f for f in copied if f.startswith("third_party/abseil-cpp/")
            and f.endswith(".cc")),
        "libyuv_generic": libyuv_generic,
        "libyuv_msvc": [f for f in copied if LIBYUV_MSVCC_RE.search(f)],
        "libyuv_gnu": [f for f in copied if LIBYUV_GNU_RE.search(f)],
        "libyuv_skipped_isa_or_jpeg": libyuv_skipped,
        "unresolved_includes": missing,
    }
    with open(os.path.join(out_root, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)

    print("vendored %d files into %s" % (len(copied), out_root))
    print("common dc .cc: %d | win: %d | x11: %d | mac: %d (+sdk objc %d) | "
          "pipewire: %d | runtime .cc: %d | absl .cc: %d | libyuv: %d"
          % (len(common_dc), len(win_dc), len(x11_dc) + len(linux_entries),
             len(mac_cc) + len(mac_mm), len(sdk_objc),
             len(pipewire_dc) + len(portal_cc), len(runtime_cc),
             len(summary["absl_sources"]),
             len(libyuv_generic) + len(summary["libyuv_msvc"]) +
             len(summary["libyuv_gnu"])))
    if missing:
        print("NOTE: %d unresolved include strings (SDK headers etc.)" % len(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
