#include "core/input_injector.h"

#include <windows.h>

#include <atomic>

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
      if (ev.pressed) {
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
    case InputEvent::Type::Key: {
      input.type = INPUT_KEYBOARD;
      input.ki.wVk = static_cast<WORD>(ev.keyCode);
      input.ki.dwFlags = ev.extended ? KEYEVENTF_EXTENDEDKEY : 0;
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
