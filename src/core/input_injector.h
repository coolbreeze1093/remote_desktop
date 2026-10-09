// remote_desktop — injects InputEvents on the host machine (Windows SendInput).
#ifndef REMOTE_DESKTOP_CORE_INPUT_INJECTOR_H_
#define REMOTE_DESKTOP_CORE_INPUT_INJECTOR_H_

#include <atomic>

#include "core/input_event.h"

namespace rd {

// Maps normalised coordinates in the captured frame to absolute desktop
// coordinates and injects mouse/keyboard events. Returns false on rejection.
// Geometry is written from the capture thread (onGeometry) and read from the
// UI thread (inject), hence the atomics.
class InputInjector {
 public:
  // Call when the capture geometry changes.
  void setFrameOrigin(int x, int y) {
    ox_.store(x, std::memory_order_relaxed);
    oy_.store(y, std::memory_order_relaxed);
  }
  void setFrameSize(int w, int h) {
    fw_.store(w, std::memory_order_relaxed);
    fh_.store(h, std::memory_order_relaxed);
  }

  bool inject(const InputEvent& ev) const;

 private:
  std::atomic_int ox_{0};
  std::atomic_int oy_{0};
  std::atomic_int fw_{0};
  std::atomic_int fh_{0};
};

}  // namespace rd

#endif
