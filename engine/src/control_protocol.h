// rdc — DataChannel 控制协议 v2：定长二进制消息（替代旧 QJson 文本协议）。
//
// 帧格式（全部小端）：
//   偏移  大小  字段
//   0     1    magic   = 0x52 ('R')
//   1     1    version = 1
//   2     1    type
//   3     1    reserved
//   4     4    payload_len (u32)
//   8     N    payload
//
// 纯函数、无全局状态，可独立单测（tests/protocol_test.cc）。未知 type /
// 坏 magic / 长度不符一律返回 false，由调用方丢弃并计数。
#ifndef RDC_SRC_CONTROL_PROTOCOL_H_
#define RDC_SRC_CONTROL_PROTOCOL_H_

#include <cstdint>
#include <cstring>
#include <vector>

#include "rdc/engine.h"

namespace rdc {
namespace ctrl {

constexpr uint8_t kMagic = 0x52;
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 8;

enum class Type : uint8_t {
  Input = 1,        // Viewer -> Host，payload 32B 定长
  FrameMeta = 2,    // Host -> Viewer，{u16 w, u16 h}
  VideoStats = 3,   // Viewer -> Host，{u32 recv, u32 dec, u32 fail}
  QualityMode = 4,  // Viewer -> Host，{u8 mode}
  KeyframeReq = 5,  // Viewer -> Host，空 payload（手动补 IDR，配合 RTCP PLI）
  SecureDesktop = 6,  // Host -> Viewer，{u8 secure, u8 capturable} 锁屏/UAC 状态
};

// InputEvent 定长 payload 大小：1+8+8+4+1+4+4+1 = 31，对齐到 32。
constexpr size_t kInputPayloadSize = 32;

// ---- 小端读写助手（显式字节序，跨平台稳定） ----

inline void Put8(std::vector<uint8_t>& b, size_t& o, uint8_t v) {
  b[o++] = v;
}
inline void Put16(std::vector<uint8_t>& b, size_t& o, uint16_t v) {
  b[o++] = static_cast<uint8_t>(v & 0xFF);
  b[o++] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
inline void Put32(std::vector<uint8_t>& b, size_t& o, uint32_t v) {
  b[o++] = static_cast<uint8_t>(v & 0xFF);
  b[o++] = static_cast<uint8_t>((v >> 8) & 0xFF);
  b[o++] = static_cast<uint8_t>((v >> 16) & 0xFF);
  b[o++] = static_cast<uint8_t>((v >> 24) & 0xFF);
}
inline void PutF64(std::vector<uint8_t>& b, size_t& o, double v) {
  uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(v), "double must be 64-bit");
  std::memcpy(&bits, &v, sizeof(bits));
  Put32(b, o, static_cast<uint32_t>(bits & 0xFFFFFFFFu));
  Put32(b, o, static_cast<uint32_t>((bits >> 32) & 0xFFFFFFFFu));
}
inline uint8_t Get8(const uint8_t* p, size_t& o) { return p[o++]; }
inline uint16_t Get16(const uint8_t* p, size_t& o) {
  const uint16_t v = static_cast<uint16_t>(p[o]) |
                     static_cast<uint16_t>(static_cast<uint16_t>(p[o + 1]) << 8);
  o += 2;
  return v;
}
inline uint32_t Get32(const uint8_t* p, size_t& o) {
  const uint32_t v = static_cast<uint32_t>(p[o]) |
                     (static_cast<uint32_t>(p[o + 1]) << 8) |
                     (static_cast<uint32_t>(p[o + 2]) << 16) |
                     (static_cast<uint32_t>(p[o + 3]) << 24);
  o += 4;
  return v;
}
inline double GetF64(const uint8_t* p, size_t& o) {
  const uint64_t bits =
      static_cast<uint64_t>(Get32(p, o)) |
      (static_cast<uint64_t>(Get32(p, o)) << 32);
  double v = 0;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

inline void AppendHeader(std::vector<uint8_t>& out, Type t, size_t payloadLen) {
  out.resize(kHeaderSize + payloadLen, 0);
  out[0] = kMagic;
  out[1] = kVersion;
  out[2] = static_cast<uint8_t>(t);
  out[3] = 0;
  size_t o = 4;
  Put32(out, o, static_cast<uint32_t>(payloadLen));
}

// ---- 编码 ----

inline std::vector<uint8_t> EncodeInputEvent(const InputEvent& ev) {
  std::vector<uint8_t> out;
  AppendHeader(out, Type::Input, kInputPayloadSize);
  size_t o = kHeaderSize;
  Put8(out, o, static_cast<uint8_t>(ev.type));
  PutF64(out, o, ev.nx);
  PutF64(out, o, ev.ny);
  Put32(out, o, static_cast<uint32_t>(static_cast<int32_t>(ev.button)));
  Put8(out, o, ev.pressed ? 1 : 0);
  Put32(out, o, static_cast<uint32_t>(static_cast<int32_t>(ev.wheelDelta)));
  Put32(out, o, ev.keyCode);
  Put8(out, o, ev.extended ? 1 : 0);
  return out;  // 尾部补零到 32B
}

inline std::vector<uint8_t> EncodeFrameMeta(int width, int height) {
  std::vector<uint8_t> out;
  AppendHeader(out, Type::FrameMeta, 4);
  size_t o = kHeaderSize;
  Put16(out, o, static_cast<uint16_t>(width));
  Put16(out, o, static_cast<uint16_t>(height));
  return out;
}

inline std::vector<uint8_t> EncodeVideoStats(uint32_t received, uint32_t decoded,
                                             uint32_t failed) {
  std::vector<uint8_t> out;
  AppendHeader(out, Type::VideoStats, 12);
  size_t o = kHeaderSize;
  Put32(out, o, received);
  Put32(out, o, decoded);
  Put32(out, o, failed);
  return out;
}

inline std::vector<uint8_t> EncodeQualityMode(int mode) {
  std::vector<uint8_t> out;
  AppendHeader(out, Type::QualityMode, 1);
  size_t o = kHeaderSize;
  Put8(out, o, static_cast<uint8_t>(mode));
  return out;
}

inline std::vector<uint8_t> EncodeKeyframeReq() {
  std::vector<uint8_t> out;
  AppendHeader(out, Type::KeyframeReq, 0);
  return out;
}

// Host 输入桌面切到/离开安全桌面（锁屏、UAC）时通知 Viewer：
// secure=true 表示已锁屏/弹 UAC；capturable=false 表示宿主进程打不开
// 安全桌面（未提权），画面与控制会中断，解锁后自动恢复。
inline std::vector<uint8_t> EncodeSecureDesktopState(bool secure,
                                                     bool capturable) {
  std::vector<uint8_t> out;
  AppendHeader(out, Type::SecureDesktop, 2);
  size_t o = kHeaderSize;
  Put8(out, o, secure ? 1 : 0);
  Put8(out, o, capturable ? 1 : 0);
  return out;
}

// ---- 解码 ----

struct Message {
  Type type = Type::Input;
  InputEvent input{};      // Type::Input
  int metaWidth = 0;       // Type::FrameMeta
  int metaHeight = 0;
  uint32_t statsReceived = 0;  // Type::VideoStats
  uint32_t statsDecoded = 0;
  uint32_t statsFailed = 0;
  int qualityMode = 0;     // Type::QualityMode
  bool secureDesktop = false;      // Type::SecureDesktop
  bool secureCapturable = false;   // Type::SecureDesktop
};

// 严格校验 magic/version/长度；成功返回 true 并填充 out。
inline bool Decode(const uint8_t* data, size_t size, Message& out) {
  if (!data || size < kHeaderSize) {
    return false;
  }
  if (data[0] != kMagic || data[1] != kVersion) {
    return false;
  }
  const uint8_t type = data[2];
  size_t o = 4;
  const uint32_t len = Get32(data, o);
  if (kHeaderSize + len != size) {
    return false;  // DataChannel 消息天然成帧，长度必须严丝合缝
  }
  const uint8_t* payload = data + kHeaderSize;
  switch (static_cast<Type>(type)) {
    case Type::Input: {
      if (len != kInputPayloadSize) {
        return false;
      }
      size_t p = 0;
      const uint8_t t = Get8(payload, p);
      if (t > 3) {
        return false;
      }
      out.type = Type::Input;
      out.input.type = static_cast<InputEvent::Type>(t);
      out.input.nx = GetF64(payload, p);
      out.input.ny = GetF64(payload, p);
      out.input.button =
          static_cast<int>(static_cast<int32_t>(Get32(payload, p)));
      out.input.pressed = Get8(payload, p) != 0;
      out.input.wheelDelta =
          static_cast<int>(static_cast<int32_t>(Get32(payload, p)));
      out.input.keyCode = Get32(payload, p);
      out.input.extended = Get8(payload, p) != 0;
      return true;
    }
    case Type::FrameMeta: {
      if (len != 4) {
        return false;
      }
      size_t p = 0;
      out.type = Type::FrameMeta;
      out.metaWidth = Get16(payload, p);
      out.metaHeight = Get16(payload, p);
      return true;
    }
    case Type::VideoStats: {
      if (len != 12) {
        return false;
      }
      size_t p = 0;
      out.type = Type::VideoStats;
      out.statsReceived = Get32(payload, p);
      out.statsDecoded = Get32(payload, p);
      out.statsFailed = Get32(payload, p);
      return true;
    }
    case Type::QualityMode: {
      if (len != 1) {
        return false;
      }
      out.type = Type::QualityMode;
      out.qualityMode = payload[0];
      return true;
    }
    case Type::KeyframeReq: {
      if (len != 0) {
        return false;
      }
      out.type = Type::KeyframeReq;
      return true;
    }
    case Type::SecureDesktop: {
      if (len != 2) {
        return false;
      }
      out.type = Type::SecureDesktop;
      out.secureDesktop = payload[0] != 0;
      out.secureCapturable = payload[1] != 0;
      return true;
    }
    default:
      return false;  // 未知 type：调用方丢弃并计数
  }
}

}  // namespace ctrl
}  // namespace rdc
#endif  // RDC_SRC_CONTROL_PROTOCOL_H_
