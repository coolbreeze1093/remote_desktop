// remote_desktop — 被控端光标状态轮询（include_cursor=false 后光标不再烤进
// 视频帧，改由控制通道单独送达主控端本地渲染）。
//
// 用法：宿主持有一个实例，把指针交给 ScreenCaptureThread（采集线程每帧
// Poll 一次）；注入线程在 SendInput 之后立即 Poll 一次，让主控端不等下一
// 采集帧就看到位置。线程安全：Poll 可从多个已附着输入桌面的线程并发调用。
//
// 桌面归属：GetCursorInfo 返回调用线程所属桌面的全局光标。采集线程与注入
// 线程都已被附着到当前输入桌面（见 input_desktop.h），因此本类不做附着——
// 由调用方保证在自己的线程上调用时桌面正确。
#ifndef REMOTE_DESKTOP_CORE_CURSOR_MONITOR_H_
#define REMOTE_DESKTOP_CORE_CURSOR_MONITOR_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include <windows.h>

namespace rd {

// 一帧光标位图：预乘 alpha 的 ARGB（内存序 BGRA，与 GDI/Qt 布局一致），
// 原生像素尺寸（主控端按视频缩放比同步缩放显示）。
struct CursorShapeData {
  int width = 0;
  int height = 0;
  int hotspotX = 0;
  int hotspotY = 0;
  std::vector<uint8_t> bgra;  // width*height*4
};

// 轮询当前输入桌面的光标位置与形态，变化时经回调上报：
//   - 位置：归一化坐标 [0..1]（相对采集帧；光标在帧外时照发，主控端自行
//     越界裁剪），仅几何已知且值变化时回调。
//   - 形态：HCURSOR 句柄变化（含首次）才重新转换位图；光标隐藏
//     （flags=0 / CURSOR_SUPPRESSED）仅影响 visible，不丢失已缓存的形态。
class CursorMonitor {
 public:
  using PositionFn = std::function<void(double nx, double ny, bool visible)>;
  using ShapeFn = std::function<void(const CursorShapeData& shape)>;

  // 采集帧几何（onGeometry 同步；w/h<=0 时位置不上报）。原子字段，采集
  // 线程写、Poll 读。
  void setFrameGeometry(int ox, int oy, int w, int h) {
    ox_.store(ox, std::memory_order_relaxed);
    oy_.store(oy, std::memory_order_relaxed);
    fw_.store(w, std::memory_order_relaxed);
    fh_.store(h, std::memory_order_relaxed);
  }

  void setCallbacks(PositionFn onPos, ShapeFn onShape) {
    std::lock_guard<std::mutex> lk(mtx_);
    onPos_ = std::move(onPos);
    onShape_ = std::move(onShape);
  }

  // 轮询一次。force=true 时无论是否变化都重发当前位置与形态（连接建立/
  // 重建后补发，主控端不必等下一次变化）。
  void Poll(bool force = false);

 private:
  static bool ConvertHCursor(HCURSOR cursor, CursorShapeData* out);

  std::mutex mtx_;
  PositionFn onPos_;
  ShapeFn onShape_;

  std::atomic_int ox_{0}, oy_{0}, fw_{0}, fh_{0};

  // 上次上报的状态（去重）。
  bool hasGeomPos_ = false;  // 几何有效期间是否已上报过位置
  double lastNx_ = 0, lastNy_ = 0;
  bool lastVisible_ = false;
  bool hasLastVisible_ = false;
  HCURSOR lastCursor_ = nullptr;  // 已转换并上报的句柄
};

}  // namespace rd

#endif
