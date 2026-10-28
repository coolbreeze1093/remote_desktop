#include "core/cursor_monitor.h"

#include <cstring>
#include <memory>

#include "debug_log.h"

namespace rd {

namespace {

// HCURSOR → 预乘 ARGB 的转换改编自 WebRTC
// modules/desktop_capture/win/cursor.cc（BSD 许可，Copyright (c) 2013 The
// WebRTC project authors）。逻辑：ICONINFO 拿掩码/彩色位图与热点；彩色位图
// 无 alpha 通道时按 AND 掩码重建透明度（XOR 反色不支持，置黑并加白描边）；
// 最后预乘 alpha。

constexpr uint32_t kPixelRgbaBlack = 0xff000000u;
constexpr uint32_t kPixelRgbaWhite = 0xffffffffu;
constexpr uint32_t kPixelRgbaTransparent = 0u;
constexpr uint32_t kPixelRgbWhite = 0xffffffu;  // RGB(255,255,255)

// 32bpp 位图的 alpha 通道是否存在非零（GetDIBits 不告知有无 alpha）。
bool HasAlphaChannel(const uint32_t* data, int width, int height) {
  for (int i = 0; i < width * height; ++i) {
    if ((data[i] & 0xff000000u) != 0) {
      return true;
    }
  }
  return false;
}

// 给单色光标加白描边（深色背景可见性，同 WebRTC）。
void AddCursorOutline(int width, int height, uint32_t* data) {
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      uint32_t& px = data[y * width + x];
      if (px != kPixelRgbaTransparent) {
        continue;
      }
      const bool nextToBlack =
          (y > 0 && data[(y - 1) * width + x] == kPixelRgbaBlack) ||
          (y < height - 1 && data[(y + 1) * width + x] == kPixelRgbaBlack) ||
          (x > 0 && data[y * width + x - 1] == kPixelRgbaBlack) ||
          (x < width - 1 && data[y * width + x + 1] == kPixelRgbaBlack);
      if (nextToBlack) {
        px = kPixelRgbaWhite;
      }
    }
  }
}

// RGB 预乘 alpha（Qt Format_ARGB32_Premultiplied 与 GDI AlphaBlend 同口径）。
void AlphaMul(uint32_t* data, int width, int height) {
  for (int i = 0; i < width * height; ++i) {
    const uint32_t px = data[i];
    const uint32_t a = (px >> 24) & 0xff;
    const uint32_t b = ((px >> 16) & 0xff) * a / 255;
    const uint32_t g = ((px >> 8) & 0xff) * a / 255;
    const uint32_t r = (px & 0xff) * a / 255;
    data[i] = (a << 24) | (b << 16) | (g << 8) | r;
  }
}

}  // namespace

bool CursorMonitor::ConvertHCursor(HCURSOR cursor, CursorShapeData* out) {
  ICONINFO ii = {};
  if (!GetIconInfo(cursor, &ii)) {
    return false;
  }
  // BITMAP 句柄随取随删（DeleteObject 对空句柄是无害无操作）。
  struct ScopedBitmap {
    HBITMAP h;
    ~ScopedBitmap() {
      if (h) {
        DeleteObject(h);
      }
    }
  } maskBitmap{ii.hbmMask}, colorBitmap{ii.hbmColor};

  BITMAP maskInfo = {};
  if (!GetObject(ii.hbmMask, sizeof(maskInfo), &maskInfo)) {
    return false;
  }
  int width = maskInfo.bmWidth;
  int height = maskInfo.bmHeight;
  if (width <= 0 || height <= 0 || width > 1024 || height > 2048) {
    return false;  // 异常尺寸防御（height 含单色的 AND+XOR 两段）
  }

  HDC dc = GetDC(nullptr);
  if (!dc) {
    return false;
  }
  struct ScopedDc {
    HDC h;
    ~ScopedDc() { ReleaseDC(nullptr, h); }
  } scopedDc{dc};

  // 掩码转 32bpp 顶行在下→上排布（负 height = top-down）。
  std::unique_ptr<uint32_t[]> maskData(new uint32_t[width * height]);
  BITMAPV5HEADER bmi = {};
  bmi.bV5Size = sizeof(bmi);
  bmi.bV5Width = width;
  bmi.bV5Height = -height;
  bmi.bV5Planes = 1;
  bmi.bV5BitCount = 32;
  bmi.bV5Compression = BI_RGB;
  if (!GetDIBits(dc, ii.hbmMask, 0, height, maskData.get(),
                 reinterpret_cast<BITMAPINFO*>(&bmi), DIB_RGB_COLORS)) {
    return false;
  }

  bool hasAlpha = false;
  const bool isColor = ii.hbmColor != nullptr;
  std::unique_ptr<uint32_t[]> image(new uint32_t[width * height]);
  if (isColor) {
    if (!GetDIBits(dc, ii.hbmColor, 0, height, image.get(),
                   reinterpret_cast<BITMAPINFO*>(&bmi), DIB_RGB_COLORS)) {
      return false;
    }
    hasAlpha = HasAlphaChannel(image.get(), width, height);
  } else {
    // 单色光标：掩码含 AND+XOR 两段（height 为两倍）。XOR 段当彩色用。
    height /= 2;
    std::memcpy(image.get(), maskData.get() + width * height,
                static_cast<size_t>(width) * height * 4);
  }

  if (!hasAlpha) {
    // 无 alpha 通道：按 AND 掩码重建透明度（组合表见 WebRTC cursor.cc）。
    bool addOutline = false;
    for (int i = 0; i < width * height; ++i) {
      const uint32_t m = maskData[i] & 0xffffffu;  // GetDIBits 置 alpha=0
      uint32_t& dst = image[i];
      if (m == kPixelRgbWhite) {
        if (dst != 0) {
          addOutline = true;  // XOR 反色：置黑并描边
          dst = kPixelRgbaBlack;
        } else {
          dst = kPixelRgbaTransparent;
        }
      } else {
        dst = kPixelRgbaBlack ^ dst;
      }
    }
    if (addOutline) {
      AddCursorOutline(width, height, image.get());
    }
  }

  AlphaMul(image.get(), width, height);

  out->width = width;
  out->height = height;
  out->hotspotX = ii.xHotspot;
  out->hotspotY = ii.yHotspot;
  const size_t bytes = static_cast<size_t>(width) * height * 4;
  out->bgra.resize(bytes);
  std::memcpy(out->bgra.data(), image.get(), bytes);
  return true;
}

void CursorMonitor::Poll(bool force) {
  std::lock_guard<std::mutex> lk(mtx_);
  if (!onPos_ && !onShape_) {
    return;
  }

  CURSORINFO ci = {};
  ci.cbSize = sizeof(ci);
  if (!GetCursorInfo(&ci)) {
    return;
  }
  const bool visible = (ci.flags & CURSOR_SHOWING) != 0;

  // ---- 位置（几何已知且变化才发） ----
  const int ox = ox_.load(std::memory_order_relaxed);
  const int oy = oy_.load(std::memory_order_relaxed);
  const int fw = fw_.load(std::memory_order_relaxed);
  const int fh = fh_.load(std::memory_order_relaxed);
  if (fw > 0 && fh > 0) {
    const double nx = (ci.ptScreenPos.x - ox) / static_cast<double>(fw);
    const double ny = (ci.ptScreenPos.y - oy) / static_cast<double>(fh);
    const bool posChanged = !hasGeomPos_ || force || nx != lastNx_ ||
                            ny != lastNy_ ||
                            (hasLastVisible_ && visible != lastVisible_);
    if (posChanged && onPos_) {
      hasGeomPos_ = true;
      lastNx_ = nx;
      lastNy_ = ny;
      onPos_(nx, ny, visible);
    }
  } else {
    hasGeomPos_ = false;
  }
  hasLastVisible_ = true;
  lastVisible_ = visible;

  // ---- 形态（句柄变化才转换；force 用于连接建立后重发当前形态） ----
  if (!visible || (ci.hCursor == lastCursor_ && !force)) {
    return;
  }
  CursorShapeData shape;
  if (!ConvertHCursor(ci.hCursor, &shape)) {
    return;  // 转换失败保留旧句柄缓存，下一轮重试同句柄
  }
  lastCursor_ = ci.hCursor;
  if (onShape_) {
    onShape_(shape);
  }
}

}  // namespace rd
