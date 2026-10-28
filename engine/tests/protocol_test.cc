// rdc — 控制协议 v2 往返单测：编码→解码逐字段一致；坏 magic/截断/长度不符/
// 未知 type 一律拒绝；随机字节永不崩溃。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "src/control_protocol.h"

using namespace rdc;
using namespace rdc::ctrl;

static int failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                         \
    }                                                                     \
  } while (0)

static bool DecodeOk(const std::vector<uint8_t>& bytes, Message& out) {
  return Decode(bytes.data(), bytes.size(), out);
}

int main() {
  // ---- InputEvent 往返 ----
  {
    InputEvent ev;
    ev.type = InputEvent::Type::MouseButton;
    ev.nx = 0.375;
    ev.ny = 0.9;
    ev.button = 2;
    ev.pressed = true;
    ev.wheelDelta = -120;
    ev.keyCode = 0x41;
    ev.extended = true;
    const auto bytes = EncodeInputEvent(ev);
    CHECK(bytes.size() == kHeaderSize + kInputPayloadSize);
    CHECK(bytes[0] == kMagic && bytes[1] == kVersion);
    CHECK(bytes[2] == static_cast<uint8_t>(Type::Input));
    Message m;
    CHECK(DecodeOk(bytes, m));
    CHECK(m.type == Type::Input);
    CHECK(m.input.type == ev.type);
    CHECK(m.input.nx == ev.nx);
    CHECK(m.input.ny == ev.ny);
    CHECK(m.input.button == ev.button);
    CHECK(m.input.pressed == ev.pressed);
    CHECK(m.input.wheelDelta == ev.wheelDelta);
    CHECK(m.input.keyCode == ev.keyCode);
    CHECK(m.input.extended == ev.extended);
    CHECK(m.input.timestampUs == 0);  // 时间戳不跨线传输
  }
  // ---- 各类型枚举值往返 ----
  {
    for (int t = 0; t <= 5; ++t) {  // 含后向兼容追加的 MouseHWheel/KeyText
      InputEvent ev;
      ev.type = static_cast<InputEvent::Type>(t);
      ev.nx = 1.0 / (t + 1);
      Message m;
      CHECK(DecodeOk(EncodeInputEvent(ev), m));
      CHECK(m.input.type == ev.type);
    }
  }
  // ---- FrameMeta ----
  {
    Message m;
    CHECK(DecodeOk(EncodeFrameMeta(1920, 1080), m));
    CHECK(m.type == Type::FrameMeta);
    CHECK(m.metaWidth == 1920 && m.metaHeight == 1080);
  }
  // ---- VideoStats ----
  {
    Message m;
    CHECK(DecodeOk(EncodeVideoStats(1234, 1200, 7), m));
    CHECK(m.type == Type::VideoStats);
    CHECK(m.statsReceived == 1234 && m.statsDecoded == 1200 &&
          m.statsFailed == 7);
  }
  // ---- QualityMode ----
  {
    Message m;
    CHECK(DecodeOk(EncodeQualityMode(3), m));
    CHECK(m.type == Type::QualityMode && m.qualityMode == 3);
  }
  // ---- KeyframeReq（空 payload）----
  {
    Message m;
    const auto bytes = EncodeKeyframeReq();
    CHECK(bytes.size() == kHeaderSize);
    CHECK(DecodeOk(bytes, m));
    CHECK(m.type == Type::KeyframeReq);
  }
  // ---- SecureDesktop（锁屏/UAC 状态）----
  {
    for (int s = 0; s <= 1; ++s) {
      for (int c = 0; c <= 1; ++c) {
        Message m;
        const auto bytes = EncodeSecureDesktopState(s != 0, c != 0);
        CHECK(bytes.size() == kHeaderSize + 2);
        CHECK(bytes[2] == static_cast<uint8_t>(Type::SecureDesktop));
        CHECK(DecodeOk(bytes, m));
        CHECK(m.type == Type::SecureDesktop);
        CHECK(m.secureDesktop == (s != 0));
        CHECK(m.secureCapturable == (c != 0));
      }
    }
    // payload 长度错误必须拒绝
    auto bad = EncodeSecureDesktopState(true, false);
    bad.pop_back();
    Message m;
    CHECK(!DecodeOk(bad, m));
  }
  // ---- 光标位置（f32 归一化坐标）----
  {
    const auto bytes = EncodeCursorPosition(0.25, 0.75, true);
    CHECK(bytes.size() == kHeaderSize + 9);
    CHECK(bytes[2] == static_cast<uint8_t>(Type::CursorPos));
    Message m;
    CHECK(DecodeOk(bytes, m));
    CHECK(m.type == Type::CursorPos);
    CHECK(m.cursorVisible);
    // f32 精度足够 4K 归一化坐标（1/3840 ≈ 2.6e-4）
    CHECK((m.cursorNx - 0.25) < 1e-6 && (0.25 - m.cursorNx) < 1e-6);
    CHECK((m.cursorNy - 0.75) < 1e-6 && (0.75 - m.cursorNy) < 1e-6);
    // 负坐标（多屏光标在采集屏左侧）往返
    const auto neg = EncodeCursorPosition(-0.5, 1.25, false);
    CHECK(DecodeOk(neg, m));
    CHECK(!m.cursorVisible);
    CHECK(m.cursorNx < -0.4999 && m.cursorNx > -0.5001);
    auto bad = EncodeCursorPosition(0, 0, true);
    bad.pop_back();
    CHECK(!DecodeOk(bad, m));
  }
  // ---- 光标形态（BGRA 位图往返 + 尺寸校验）----
  {
    CursorShape s;
    s.width = 4;
    s.height = 2;
    s.hotspotX = 1;
    s.hotspotY = 2;
    s.bgra.assign(static_cast<size_t>(s.width) * s.height * 4, 0x7f);
    const auto bytes = EncodeCursorShape(s);
    CHECK(bytes.size() == kHeaderSize + 8 + s.bgra.size());
    CHECK(bytes[2] == static_cast<uint8_t>(Type::CursorShape));
    Message m;
    CHECK(DecodeOk(bytes, m));
    CHECK(m.type == Type::CursorShape && m.cursorShape);
    CHECK(m.cursorShape->width == 4 && m.cursorShape->height == 2);
    CHECK(m.cursorShape->hotspotX == 1 && m.cursorShape->hotspotY == 2);
    CHECK(m.cursorShape->bgra.size() == s.bgra.size());
    CHECK(m.cursorShape->bgra.back() == 0x7f);
    // 截断 / 尺寸不符 / 超上限一律拒绝
    auto bad = bytes;
    bad.pop_back();
    CHECK(!DecodeOk(bad, m));
    auto badHeader = bytes;
    badHeader[kHeaderSize] = 0xFF;  // width 低字节改大 → 与 payload 长度不符
    CHECK(!DecodeOk(badHeader, m));
    CursorShape tooBig;
    tooBig.width = kCursorShapeMaxDim + 1;
    tooBig.height = 1;
    tooBig.bgra.assign(
        static_cast<size_t>(tooBig.width) * tooBig.height * 4, 0);
    CHECK(EncodeCursorShape(tooBig).empty());
    CursorShape emptyShape;
    CHECK(EncodeCursorShape(emptyShape).empty());
  }
  // ---- 拒绝路径 ----
  {
    const auto good = EncodeQualityMode(2);
    Message m;
    // 坏 magic
    auto bad = good;
    bad[0] = 'X';
    CHECK(!DecodeOk(bad, m));
    // 坏 version
    bad = good;
    bad[1] = 99;
    CHECK(!DecodeOk(bad, m));
    // 长度不符（截断）
    CHECK(!Decode(good.data(), good.size() - 1, m));
    // 长度不符（多一字节）
    auto bad2 = good;
    bad2.push_back(0);
    CHECK(!DecodeOk(bad2, m));
    // 未知 type
    auto bad3 = good;
    bad3[2] = 200;
    CHECK(!DecodeOk(bad3, m));
    // 空 / null
    CHECK(!Decode(nullptr, 8, m));
    CHECK(!Decode(good.data(), 0, m));
    // Input payload 长度错误
    auto bad4 = EncodeInputEvent(InputEvent{});
    bad4.resize(kHeaderSize + kInputPayloadSize - 1);
    CHECK(!DecodeOk(bad4, m));
  }
  // ---- 随机字节永不崩溃 ----
  {
    std::srand(42);
    Message m;
    for (int i = 0; i < 10000; ++i) {
      std::vector<uint8_t> bytes(1 + std::rand() % 128);
      for (auto& b : bytes) {
        b = static_cast<uint8_t>(std::rand());
      }
      // 强制大概率出现合法 magic/version，专攻后续解析路径
      if (bytes.size() >= 2 && (i % 4 == 0)) {
        bytes[0] = kMagic;
        bytes[1] = kVersion;
      }
      (void)Decode(bytes.data(), bytes.size(), m);
    }
  }

  if (failures == 0) {
    std::fprintf(stderr, "rdc_protocol_test: ALL PASS\n");
    return 0;
  }
  std::fprintf(stderr, "rdc_protocol_test: %d FAILURES\n", failures);
  return 1;
}
