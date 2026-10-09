// remote_desktop — adaptive video quality ladder.
//
// The captured (native) monitor size defines three levels:
//   0 流畅  = 1/2 scale, ~4 Mbps
//   1 高清  = 3/4 scale, ~12 Mbps
//   2 超清  = full scale, ~24 Mbps
// Auto mode starts at the sharpest and steps down on pressure (slow encode,
// decoder-reported loss, bandwidth starvation), climbing back after a clean
// streak. Manual modes pin a level.
//
// Threading: all methods run on the encoder thread (hostLoop consumer loop);
// the UI thread only feeds mode/stats through RemoteDesktopSession's atomics,
// which the per-second evaluate() tick drains.
#ifndef REMOTE_DESKTOP_SESSION_VIDEO_QUALITY_H_
#define REMOTE_DESKTOP_SESSION_VIDEO_QUALITY_H_

#include <algorithm>
#include <cstdint>

#include "../debug_log.h"

namespace rd {

class VideoQualityController {
 public:
  static constexpr int kLevels = 3;

  // User-facing mode (Viewer UI): 0=auto, 1=超清, 2=高清, 3=流畅.
  // Returns the pinned level, or -1 for auto.
  static int levelForMode(int mode) {
    switch (mode) {
      case 1: return 2;
      case 2: return 1;
      case 3: return 0;
      default: return -1;
    }
  }

  static int bitrateKbpsFor(int level) {
    static const int kKbps[kLevels] = {4000, 12000, 24000};
    return kKbps[std::min(level, kLevels - 1)];
  }

  // Encode fps per level: sharper levels trade frame rate for pixels so a
  // single-threaded encoder can keep up (screen content tolerates low fps;
  // input injection is not throttled by this).
  static int fpsFor(int level) {
    static const int kFps[kLevels] = {24, 12, 8};
    return kFps[std::min(level, kLevels - 1)];
  }

  // Per-frame pipeline budget (ms) used by the auto policy.
  static double budgetMsFor(int level) {
    return 1000.0 / fpsFor(level);
  }

  void setNativeSize(int w, int h) {
    nativeW_ = w;
    nativeH_ = h;
  }

  // Even-rounded encode size for the level (chroma needs /2, OpenH264 needs
  // even dims).
  void sizeFor(int level, int* w, int* h) const {
    static const double kScale[kLevels] = {0.5, 0.75, 1.0};
    const int maxW = nativeW_ > 0 ? nativeW_ : 1280;
    const int maxH = nativeH_ > 0 ? nativeH_ : 720;
    *w = std::max(640, static_cast<int>(maxW * kScale[level]) & ~1);
    *h = std::max(360, static_cast<int>(maxH * kScale[level]) & ~1);
  }

  // Per-frame pipeline cost (convert+encode+send), ms. EMA.
  void onEncodedFrame(double pipelineMs) {
    emaMs_ = emaMs_ < 0 ? pipelineMs : emaMs_ * 0.9 + pipelineMs * 0.1;
  }

  // Viewer per-second deltas (drained from session atomics).
  void onViewerStats(int recvDelta, int decDelta, int failDelta) {
    statSeen_ = true;
    lastRecv_ = recvDelta;
    lastDec_ = decDelta;
    lastFail_ = failDelta;
  }

  // Called once per second from the capture loop. `encodedLastSecond` is how
  // many AUs went out in the same window; returns the desired level.
  int evaluate(int mode, int encodedLastSecond) {
    const int manual = levelForMode(mode);
    if (manual >= 0) {
      if (manual != level_) {
        setLevel(manual, "manual");
      }
      return level_;
    }

    // CPU pressure: per-frame cost beyond the current level's budget.
    if (emaMs_ > budgetMsFor(level_) && level_ > 0) {
      setLevel(level_ - 1, "encode slow");
      return level_;
    }

    if (statSeen_ && cooldown_ == 0) {
      // Decoder-reported loss: real packet loss, or the resolution-change
      // frames right after a ladder switch (grace period covers those).
      if (graceTicks_ > 0) {
        --graceTicks_;
      } else if (lastFail_ > 0 && level_ > 0) {
        setLevel(level_ - 1, "decoder loss");
        return level_;
      }
      // Bandwidth starvation: significantly fewer frames arrived than sent.
      if (lastRecv_ + 5 < encodedLastSecond && level_ > 0) {
        setLevel(level_ - 1, "bandwidth");
        return level_;
      }
    }

    if (cooldown_ > 0) {
      --cooldown_;
      return level_;
    }

    // Climb back up after a sustained clean period (80% of the next level's
    // budget so we don't step up into immediate CPU pressure).
    if (statSeen_ && lastFail_ == 0 && lastRecv_ + 5 >= encodedLastSecond &&
        emaMs_ >= 0 && emaMs_ < budgetMsFor(level_ + 1) * 0.8 &&
        level_ < kLevels - 1) {
      if (++cleanTicks_ >= 12) {
        setLevel(level_ + 1, "clean link");
      }
    } else {
      cleanTicks_ = 0;
    }
    return level_;
  }

  int level() const { return level_; }
  double emaMs() const { return emaMs_; }

 private:
  void setLevel(int l, const char* why) {
    int w, h;
    sizeFor(l, &w, &h);
    rdDebug("quality", "level %d -> %d (%s) %dx%d @%dkbps %dfps ema=%.1fms",
            level_, l, why, w, h, bitrateKbpsFor(l), fpsFor(l), emaMs_);
    level_ = l;
    cooldown_ = 3;    // seconds before the next auto change
    graceTicks_ = 3;  // tolerate decoder resolution-change failures
    cleanTicks_ = 0;
  }

  int level_ = kLevels - 1;  // auto starts at the sharpest
  int nativeW_ = 0;
  int nativeH_ = 0;
  double emaMs_ = -1.0;
  bool statSeen_ = false;
  int lastRecv_ = 0;
  int lastDec_ = 0;
  int lastFail_ = 0;
  int cooldown_ = 0;
  int graceTicks_ = 0;
  int cleanTicks_ = 0;
};

}  // namespace rd

#endif
