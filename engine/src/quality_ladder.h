// rdc — 自适应画质阶梯（自旧 session/video_quality.h 原样迁移，rdDebug ->
// rdcDebug）。
//
// 采集显示器的原生尺寸定义三档：
//   0 流畅 = 1/2 缩放 ~4Mbps 24fps
//   1 高清 = 3/4 缩放 ~12Mbps 12fps
//   2 超清 = 原生     ~24Mbps 8fps
// 自动档从最清晰起步，遇压力降档（编码慢 / 解码端丢包 / 带宽不足），链路
// 干净 12 秒后回升；手动模式钉死档位。
//
// 线程：所有方法只在引擎编码线程上跑；Viewer 统计由控制线程经原子变量喂入，
// 由每秒一次的 evaluate() tick 消耗。
#ifndef RDC_SRC_QUALITY_LADDER_H_
#define RDC_SRC_QUALITY_LADDER_H_

#include <algorithm>

#include "rdc/log.h"

namespace rdc {

class QualityLadder {
 public:
  static constexpr int kLevels = 3;

  // 用户视角模式（Viewer UI）：0=auto 1=超清 2=高清 3=流畅。返回钉死档位，
  // auto 返回 -1。
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

  // 每档编码帧率：越清晰的档位用帧率换像素，单线程编码器才跟得上（屏幕
  // 内容低帧率可用；鼠标键盘注入不受帧率影响）。
  static int fpsFor(int level) {
    static const int kFps[kLevels] = {24, 12, 8};
    return kFps[std::min(level, kLevels - 1)];
  }

  // 自动策略用的每帧流水线预算（ms）。
  static double budgetMsFor(int level) { return 1000.0 / fpsFor(level); }

  void setNativeSize(int w, int h) {
    nativeW_ = w;
    nativeH_ = h;
  }

  // 档位对应的编码尺寸（偶数对齐：色度需 /2，OpenH264 要求偶数尺寸）。
  void sizeFor(int level, int* w, int* h) const {
    static const double kScale[kLevels] = {0.5, 0.75, 1.0};
    const int maxW = nativeW_ > 0 ? nativeW_ : 1280;
    const int maxH = nativeH_ > 0 ? nativeH_ : 720;
    *w = std::max(640, static_cast<int>(maxW * kScale[level]) & ~1);
    *h = std::max(360, static_cast<int>(maxH * kScale[level]) & ~1);
  }

  // 每帧流水线耗时（转换+编码+发送），ms，EMA。
  void onEncodedFrame(double pipelineMs) {
    emaMs_ = emaMs_ < 0 ? pipelineMs : emaMs_ * 0.9 + pipelineMs * 0.1;
  }

  // Viewer 每秒统计增量（引擎从原子变量收集后喂入）。
  void onViewerStats(int recvDelta, int decDelta, int failDelta) {
    statSeen_ = true;
    lastRecv_ = recvDelta;
    lastDec_ = decDelta;
    lastFail_ = failDelta;
  }

  // 每秒调用一次（编码循环 tick）。encodedLastSecond 是同窗口发出的 AU 数；
  // 返回期望档位。
  int evaluate(int mode, int encodedLastSecond) {
    const int manual = levelForMode(mode);
    if (manual >= 0) {
      if (manual != level_) {
        setLevel(manual, "manual");
      }
      return level_;
    }

    // CPU 压力：每帧耗时超出当前档预算。
    if (emaMs_ > budgetMsFor(level_) && level_ > 0) {
      setLevel(level_ - 1, "encode slow");
      return level_;
    }

    if (statSeen_ && cooldown_ == 0) {
      // 解码端丢包：真实丢包，或档位切换后的分辨率变更帧（宽限期已覆盖）。
      if (graceTicks_ > 0) {
        --graceTicks_;
      } else if (lastFail_ > 0 && level_ > 0) {
        setLevel(level_ - 1, "decoder loss");
        return level_;
      }
      // 带宽不足：收到的帧明显少于发出的帧。
      if (lastRecv_ + 5 < encodedLastSecond && level_ > 0) {
        setLevel(level_ - 1, "bandwidth");
        return level_;
      }
    }

    if (cooldown_ > 0) {
      --cooldown_;
      return level_;
    }

    // 链路持续干净后回升（用下一档预算的 80% 门槛，避免升档立即触发 CPU
    // 压力来回震荡）。
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
    rdcDebug("quality", "level %d -> %d (%s) %dx%d @%dkbps %dfps ema=%.1fms",
             level_, l, why, w, h, bitrateKbpsFor(l), fpsFor(l), emaMs_);
    level_ = l;
    cooldown_ = 3;    // 下次自动变更前的冷却秒数
    graceTicks_ = 3;  // 容忍分辨率切换导致的解码失败
    cleanTicks_ = 0;
  }

  int level_ = kLevels - 1;  // auto 从最清晰起步
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

}  // namespace rdc
#endif  // RDC_SRC_QUALITY_LADDER_H_
