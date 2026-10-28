// rd_host_service — 跨会话共享内存帧环形缓冲（helper 写 → 服务读）。
//
// 引擎上收进服务（session 0）后，采集仍在控制台会话的 helper 里，4K BGRA
// 原始帧（~33MB/帧 @24fps ≈ 800MB/s）命名管道传不动，共享内存是唯一现实
// 通道。两侧不同会话，必须用 Global\ 命名空间：
//
//   helper（写方，SYSTEM）按首帧几何建映射，3 槽轮转：置 writing → memcpy
//   → 填 seq（release）→ 置 ready；服务（读方）经 captureReady 通知按名打
//   开，每轮取"最新的 ready 槽"把像素指针直接交给引擎（PushVideoFrameBgra
//   同步拷贝）。
//
// 无锁正确性：元数据经 state/seq 的 release/acquire 传递，ready 后像素不变；
// 引擎拷贝一帧 <10ms，写方绕回同一槽至少隔 2 帧（24fps 下 ≥83ms），不会撕
// 裂。lastSeq 归读方对象所有、Open 时清零——helper 重建映射后 seq 重新从 1
// 计数，按读方区分才不会把新映射的帧全部判旧丢掉。
//
// 生命周期：映射对象归内核，任一侧持有句柄即存活。helper 消亡后服务侧的
// 视图仍在（读到的是最后的旧帧，靠 lastSeq+新 captureReady 才会推进），新
// helper 重建映射后服务收到 captureReady 重开即可。
#ifndef REMOTE_DESKTOP_SERVICE_FRAME_RING_H_
#define REMOTE_DESKTOP_SERVICE_FRAME_RING_H_

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace rdframe {

// Global\ 创建需要 SeCreateGlobalPrivilege（SYSTEM 服务默认持有；helper
// 启动时会显式启用一次）。普通用户令牌（SpawnHelperAsUser 兜底路径）建不
// 出来，此时推流不可用——与非 SYSTEM 本就开不了安全桌面一致，落日志即可。
//
// 映射名由写方每次采集起一个唯一名（pid+计数），经 captureReady 消息告知
// 读方。不能用固定名：分辨率变化时 helper 要重建映射，而服务侧旧视图尚未
// 释放，固定名 CreateFileMapping 会命中旧对象（尺寸不符被忽略），两侧几何
// 从此错位。唯一名让新旧映射短暂并存，服务收到 captureReady 重开新名即切换。
constexpr wchar_t kRingNamePrefix[] = L"Global\\rdc-host-frame-";

constexpr int kSlotCount = 3;
constexpr uint32_t kRingMagic = 0x52444652u;  // 'RDFR'

struct RingHeader {
  uint32_t magic;
  int32_t slotCount;
  int64_t slotBytes;  // 单槽像素区字节数（stride*height）
};

struct SlotMeta {
  std::atomic<uint32_t> state;  // 0=空 1=写入中 2=就绪
  std::atomic<uint64_t> seq;
  int32_t width;
  int32_t height;
  int32_t stride;
  int32_t pad;
};

constexpr size_t kHeaderBytes =
    sizeof(RingHeader) + sizeof(SlotMeta) * kSlotCount;

inline int64_t SlotBytes(int32_t stride, int32_t height) {
  return static_cast<int64_t>(stride) * height;
}

inline size_t MappedBytes(int64_t slotBytes) {
  return kHeaderBytes + static_cast<size_t>(slotBytes) * kSlotCount;
}

inline RingHeader* HeaderOf(void* base) {
  return static_cast<RingHeader*>(base);
}
inline SlotMeta* MetasOf(void* base) {
  return reinterpret_cast<SlotMeta*>(static_cast<char*>(base) +
                                     sizeof(RingHeader));
}
inline uint8_t* SlotPtr(void* base, int64_t slotBytes, int i) {
  return reinterpret_cast<uint8_t*>(base) + sizeof(RingHeader) +
         sizeof(SlotMeta) * kSlotCount + static_cast<size_t>(slotBytes) * i;
}

// ---- 写方（helper 采集线程）------------------------------------------------

class FrameWriter {
 public:
  FrameWriter() = default;
  FrameWriter(const FrameWriter&) = delete;
  FrameWriter& operator=(const FrameWriter&) = delete;
  ~FrameWriter() { Close(); }

  // 按唯一名与实际几何建映射；帧尺寸变化（换屏/改分辨率）由 Close+重开走。
  bool Open(const std::wstring& name, int32_t width, int32_t height,
            int32_t stride) {
    Close();
    const int64_t slotBytes = SlotBytes(stride, height);
    if (slotBytes <= 0 || name.empty()) {
      return false;
    }
    name_ = name;
    map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                              static_cast<DWORD>(MappedBytes(slotBytes)),
                              name_.c_str());
    if (!map_) {
      return false;
    }
    base_ = MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!base_) {
      Close();
      return false;
    }
    auto* hdr = HeaderOf(base_);
    hdr->magic = kRingMagic;
    hdr->slotCount = kSlotCount;
    hdr->slotBytes = slotBytes;
    SlotMeta* metas = MetasOf(base_);
    for (int i = 0; i < kSlotCount; ++i) {
      metas[i].state.store(0, std::memory_order_relaxed);
      metas[i].seq.store(0, std::memory_order_relaxed);
      metas[i].width = 0;
      metas[i].height = 0;
      metas[i].stride = 0;
      metas[i].pad = 0;
    }
    width_ = width;
    height_ = height;
    stride_ = stride;
    slotBytes_ = slotBytes;
    return true;
  }

  // 每帧调用。映射未开（首帧前 Open 失败）时静默丢弃。
  void Write(const uint8_t* pixels) {
    if (!base_) {
      return;
    }
    const int i = next_;
    SlotMeta* m = MetasOf(base_) + i;
    m->state.store(1, std::memory_order_relaxed);
    memcpy(SlotPtr(base_, slotBytes_, i), pixels, static_cast<size_t>(slotBytes_));
    m->width = width_;
    m->height = height_;
    m->stride = stride_;
    m->seq.store(++seq_, std::memory_order_relaxed);
    m->state.store(2, std::memory_order_release);
    next_ = (next_ + 1) % kSlotCount;
  }

  bool opened() const { return base_ != nullptr; }
  const std::wstring& name() const { return name_; }
  int32_t width() const { return width_; }
  int32_t height() const { return height_; }
  int32_t stride() const { return stride_; }

  void Close() {
    if (base_) {
      UnmapViewOfFile(base_);
      base_ = nullptr;
    }
    if (map_) {
      CloseHandle(map_);
      map_ = nullptr;
    }
    name_.clear();
    width_ = height_ = stride_ = 0;
    slotBytes_ = 0;
    next_ = 0;
    seq_ = 0;
  }

 private:
  HANDLE map_ = nullptr;
  void* base_ = nullptr;
  std::wstring name_;
  int32_t width_ = 0;
  int32_t height_ = 0;
  int32_t stride_ = 0;
  int64_t slotBytes_ = 0;
  int next_ = 0;
  uint64_t seq_ = 0;
};

// ---- 读方（服务帧泵）--------------------------------------------------------

class FrameReader {
 public:
  FrameReader() = default;
  FrameReader(const FrameReader&) = delete;
  FrameReader& operator=(const FrameReader&) = delete;
  ~FrameReader() { Close(); }

  bool Open(const std::wstring& name) {
    Close();
    if (name.empty()) {
      return false;
    }
    map_ = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!map_) {
      return false;
    }
    base_ = MapViewOfFile(map_, FILE_MAP_READ, 0, 0, 0);
    if (!base_) {
      Close();
      return false;
    }
    const RingHeader* hdr = HeaderOf(base_);
    if (hdr->magic != kRingMagic || hdr->slotCount != kSlotCount ||
        hdr->slotBytes <= 0) {
      Close();
      return false;
    }
    slotBytes_ = hdr->slotBytes;
    lastSeq_ = 0;  // 新映射 seq 从 1 重计（见文件头注释）
    return true;
  }

  void Close() {
    if (base_) {
      UnmapViewOfFile(base_);
      base_ = nullptr;
    }
    if (map_) {
      CloseHandle(map_);
      map_ = nullptr;
    }
    slotBytes_ = 0;
    lastSeq_ = 0;
  }

  bool opened() const { return base_ != nullptr; }

  // 把比上次消费更新的最新完整帧交给 fn(pixels, w, h, stride)（引擎同步拷
  // 贝）。没有新帧返回 false。须在确保写方不会重建映射的情况下调用（服务
  // 侧由 host_session 的锁保证）。
  template <class Fn>
  bool ConsumeNewest(Fn&& fn) {
    if (!base_) {
      return false;
    }
    SlotMeta* metas = MetasOf(base_);
    int best = -1;
    uint64_t bestSeq = lastSeq_;
    for (int i = 0; i < kSlotCount; ++i) {
      if (metas[i].state.load(std::memory_order_acquire) != 2) {
        continue;
      }
      const uint64_t s = metas[i].seq.load(std::memory_order_acquire);
      if (s > bestSeq) {
        bestSeq = s;
        best = i;
      }
    }
    if (best < 0) {
      return false;
    }
    const SlotMeta* m = metas + best;
    fn(SlotPtr(base_, slotBytes_, best), m->width, m->height, m->stride);
    lastSeq_ = bestSeq;
    return true;
  }

 private:
  HANDLE map_ = nullptr;
  void* base_ = nullptr;
  int64_t slotBytes_ = 0;
  uint64_t lastSeq_ = 0;
};

}  // namespace rdframe

#endif  // REMOTE_DESKTOP_SERVICE_FRAME_RING_H_
