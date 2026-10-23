#include "core/input_injector.h"

#include <windows.h>

#include <atomic>
#include <string>

#include "core/input_desktop.h"
#include "debug_log.h"

namespace rd {

namespace {

constexpr DWORD kButtonFlags[4] = {0, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN,
                                   MOUSEEVENTF_MIDDLEDOWN};
constexpr DWORD kButtonUpFlags[4] = {0, MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP,
                                     MOUSEEVENTF_MIDDLEUP};

}  // namespace

bool InputInjector::inject(const InputEvent& ev) const {
  const int fw = fw_.load(std::memory_order_relaxed);
  const int fh = fh_.load(std::memory_order_relaxed);
  if (fw <= 0 || fh <= 0) {
    return false;
  }

  // 锁屏/UAC 时输入桌面切到安全桌面（Winlogon），SendInput 只会落到调用
  // 线程所属桌面的输入流：先把注入线程附着到当前输入桌面。管理员进程跟随
  // 切换后可控制锁屏；未提权进程打不开安全桌面（kOpenDenied），注入失效。
  // 按线程缓存，桌面未变时只有一次探测开销。
  {
    std::wstring desktopName;
    const AttachResult att = AttachThisThreadToInputDesktop(&desktopName);
    static std::atomic<int> attDbg{0};
    if (att != AttachResult::kOnInputDesktop && attDbg.fetch_add(1) < 20) {
      // 桌面名（Default/Winlogon）都是 ASCII，窄化仅用于日志。
      const std::string narrow(desktopName.begin(), desktopName.end());
      rdDebug("inject", "attach input desktop failed (%d) name=%s", (int)att,
              narrow.c_str());
    }
  }

  // Normalised frame position -> pixel position in the captured frame ->
  // absolute virtual-desktop coordinates (0..65535 space).
  const int ox = ox_.load(std::memory_order_relaxed);
  const int oy = oy_.load(std::memory_order_relaxed);
  const int pixelX = ox + static_cast<int>(ev.nx * fw);
  const int pixelY = oy + static_cast<int>(ev.ny * fh);
  const int virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int virtualW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int virtualH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (virtualW <= 0 || virtualH <= 0) {
    return false;
  }
  const DWORD ax = static_cast<DWORD>(
      (static_cast<double>(pixelX - virtualX) / virtualW) * 65535.0);
  const DWORD ay = static_cast<DWORD>(
      (static_cast<double>(pixelY - virtualY) / virtualH) * 65535.0);

  INPUT input;
  memset(&input, 0, sizeof(input));

  bool ok = false;
  switch (ev.type) {
    case InputEvent::Type::MouseMove: {
      input.type = INPUT_MOUSE;
      input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                         MOUSEEVENTF_VIRTUALDESK;
      input.mi.dx = ax;
      input.mi.dy = ay;
      break;
    }
    case InputEvent::Type::MouseButton: {
      // Move to position first, then press/release.
      INPUT move;
      memset(&move, 0, sizeof(move));
      move.type = INPUT_MOUSE;
      move.mi.dwFlags =
          MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
      move.mi.dx = ax;
      move.mi.dy = ay;
      SendInput(1, &move, sizeof(move));

      input.type = INPUT_MOUSE;
      if (ev.button >= 4 && ev.button <= 5) {
        // 侧键（后退/前进）：XBUTTONDOWN/XUP 携带 XBUTTON1/2。
        input.mi.dwFlags = ev.pressed ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        input.mi.mouseData =
            (ev.button == 4) ? XBUTTON1 : XBUTTON2;
      } else if (ev.pressed) {
        input.mi.dwFlags =
            (ev.button >= 1 && ev.button <= 3) ? kButtonFlags[ev.button] : 0;
      } else {
        input.mi.dwFlags =
            (ev.button >= 1 && ev.button <= 3) ? kButtonUpFlags[ev.button] : 0;
      }
      break;
    }
    case InputEvent::Type::MouseWheel: {
      input.type = INPUT_MOUSE;
      input.mi.dwFlags = MOUSEEVENTF_WHEEL;
      input.mi.mouseData = static_cast<DWORD>(ev.wheelDelta);
      break;
    }
    case InputEvent::Type::MouseHWheel: {
      input.type = INPUT_MOUSE;
      input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
      input.mi.mouseData = static_cast<DWORD>(ev.wheelDelta);
      break;
    }
    case InputEvent::Type::Key: {
      input.type = INPUT_KEYBOARD;
      input.ki.wVk = static_cast<WORD>(ev.keyCode);
      input.ki.dwFlags = ev.extended ? KEYEVENTF_EXTENDEDKEY : 0;
      if (!ev.pressed) {
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
      }
      break;
    }
    case InputEvent::Type::KeyText: {
      // IME 提交文本：VK_PACKET 路径，wScan 直接携带 UTF-16 码元，
      // 目标应用收到 WM_CHAR；代理对按高低半字各发一次即可重组。
      input.type = INPUT_KEYBOARD;
      input.ki.wVk = 0;
      input.ki.wScan = static_cast<WORD>(ev.keyCode);
      input.ki.dwFlags = KEYEVENTF_UNICODE;
      if (!ev.pressed) {
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
      }
      break;
    }
    default:
      return false;
  }

  ok = SendInput(1, &input, sizeof(input)) == 1;
  {
    static std::atomic<int> dbg{0};
    if (dbg.fetch_add(1) < 10) {
      rdDebug("inject", "type=%d pixel=(%d,%d) abs=(%u,%u) frame=(%d,%d) ok=%d",
              (int)ev.type, pixelX, pixelY, (unsigned)input.mi.dx,
              (unsigned)input.mi.dy, fw, fh, (int)ok);
    }
  }
  return ok;
}

}  // namespace rd
