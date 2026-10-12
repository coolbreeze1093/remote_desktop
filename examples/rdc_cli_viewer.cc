// rdc_cli_viewer — 无 Qt 引擎 demo：主控端（Viewer）。
//
// 验证 rdc 引擎可完全脱离 Qt 使用：
//   1. 引擎 Viewer 启动 → CreateOffer → 写 CWD 下 rdc_offer.txt
//   2. 等待 rdc_answer.txt（60s）→ AcceptAnswer
//   3. OnVideoFrame 回调收解码帧：计数；--dump 首帧存 BMP
//   4. --inject 时每秒发一个鼠标移动事件（远端被控端注入路径演示）
//   5. --duration 秒后打印统计退出
//
// 用法: rdc_cli_viewer [--duration S] [--dump first.bmp] [--inject]
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "rdc/engine.h"

using namespace rdc;

namespace {

const char* StateName(State s) {
  switch (s) {
    case State::Idle: return "idle";
    case State::Gathering: return "gathering";
    case State::Connecting: return "connecting";
    case State::Connected: return "connected";
    case State::Failed: return "failed";
    case State::Closed: return "closed";
  }
  return "?";
}

void WriteFile(const std::string& path, const std::string& content) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << content;
}

bool WaitForFile(const std::string& path, std::string* out, int timeoutMs) {
  for (int waited = 0; waited < timeoutMs; waited += 200) {
    std::ifstream f(path, std::ios::binary);
    if (f) {
      std::string content((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
      while (!content.empty() &&
             (content.back() == '\n' || content.back() == '\r' ||
              content.back() == ' ' || content.back() == '\t')) {
        content.pop_back();
      }
      if (!content.empty()) {
        *out = content;
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return false;
}

// 24 位 BMP（自 BGRA 内存缓冲，bottom-up）。
void WriteBmp(const std::string& path, const uint8_t* bgra, int w, int h,
              int strideBytes) {
  const int rowBytes = w * 3;
  const int pad = (4 - (rowBytes % 4)) % 4;
  const uint32_t dataSize =
      static_cast<uint32_t>((rowBytes + pad) * static_cast<size_t>(h));
  const uint32_t fileSize = 54 + dataSize;
  std::vector<uint8_t> buf(fileSize, 0);
  auto put16 = [&](size_t o, uint16_t v) {
    buf[o] = static_cast<uint8_t>(v & 0xFF);
    buf[o + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  };
  auto put32 = [&](size_t o, uint32_t v) {
    buf[o] = static_cast<uint8_t>(v & 0xFF);
    buf[o + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    buf[o + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    buf[o + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
  };
  buf[0] = 'B';
  buf[1] = 'M';
  put32(2, fileSize);
  put32(10, 54);
  put32(14, 40);
  put32(18, static_cast<uint32_t>(w));
  put32(22, static_cast<uint32_t>(h));
  put16(26, 1);
  put16(28, 24);
  put32(34, dataSize);
  for (int r = 0; r < h; ++r) {
    const uint8_t* src =
        bgra + static_cast<size_t>(h - 1 - r) * strideBytes;  // bottom-up
    uint8_t* dst = buf.data() + 54 + static_cast<size_t>(r) * (rowBytes + pad);
    for (int x = 0; x < w; ++x) {
      dst[x * 3 + 0] = src[x * 4 + 0];  // B
      dst[x * 3 + 1] = src[x * 4 + 1];  // G
      dst[x * 3 + 2] = src[x * 4 + 2];  // R
    }
  }
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(buf.data()),
          static_cast<std::streamsize>(buf.size()));
}

}  // namespace

int main(int argc, char** argv) {
  int durationSec = 20;
  std::string dumpPath;
  bool inject = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--duration" && i + 1 < argc) {
      durationSec = std::atoi(argv[++i]);
    } else if (a == "--dump" && i + 1 < argc) {
      dumpPath = argv[++i];
    } else if (a == "--inject") {
      inject = true;
    }
  }

  auto engine = Engine::Create(Role::Viewer);
  std::atomic<int> frames{0};
  bool connected = false;
  engine->OnState([&](State s, const std::string& detail) {
    std::fprintf(stderr, "[viewer] state=%s detail=%s\n", StateName(s),
                 detail.c_str());
    std::fflush(stderr);
    if (s == State::Connected) {
      connected = true;
    }
  });
  engine->OnError([](const std::string& e) {
    std::fprintf(stderr, "[viewer] error: %s\n", e.c_str());
    std::fflush(stderr);
  });
  engine->OnVideoFrame([&](std::shared_ptr<const VideoFrame> frame) {
    if (!frame) {
      return;
    }
    const int n = frames.fetch_add(1) + 1;
    if (n == 1 && !dumpPath.empty()) {
      const int stride = frame->width * 4;
      std::vector<uint8_t> bgra(static_cast<size_t>(stride) * frame->height);
      I420ToBgra(frame->y, frame->yStride, frame->u, frame->uStride, frame->v,
                 frame->vStride, frame->width, frame->height, bgra.data(),
                 stride);
      WriteBmp(dumpPath, bgra.data(), frame->width, frame->height, stride);
      std::fprintf(stderr, "[viewer] first frame %dx%d dumped to %s\n",
                   frame->width, frame->height, dumpPath.c_str());
      std::fflush(stderr);
    }
    if (n % 60 == 1) {
      std::fprintf(stderr, "[viewer] frames=%d (%dx%d)\n", n, frame->width,
                   frame->height);
      std::fflush(stderr);
    }
  });
  if (!engine->Start()) {
    std::fprintf(stderr, "[viewer] engine start FAILED\n");
    return 1;
  }

  std::fprintf(stderr, "[viewer] creating offer (ICE gathering, up to 10s)...\n");
  std::fflush(stderr);
  const std::string offer = engine->CreateOffer();
  if (offer.empty()) {
    std::fprintf(stderr, "[viewer] offer FAILED\n");
    return 2;
  }
  WriteFile("rdc_offer.txt", offer);
  std::fprintf(stderr,
               "[viewer] offer written to rdc_offer.txt (%zu chars); "
               "waiting rdc_answer.txt (60s)...\n",
               offer.size());
  std::fflush(stderr);
  std::string answer;
  if (!WaitForFile("rdc_answer.txt", &answer, 60000)) {
    std::fprintf(stderr, "[viewer] no rdc_answer.txt\n");
    return 2;
  }
  if (!engine->AcceptAnswer(answer)) {
    std::fprintf(stderr, "[viewer] accept answer FAILED\n");
    return 2;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(durationSec);
  int tick = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ++tick;
    if (inject && connected) {
      // 演示输入外发：横扫一遍鼠标位置。
      InputEvent ev;
      ev.type = InputEvent::Type::MouseMove;
      ev.nx = (tick % 100) / 100.0;
      ev.ny = 0.5;
      engine->SendInput(ev);
    }
    if (tick % 5 == 0) {
      const Stats st = engine->GetStats();
      std::fprintf(stderr, "[viewer] t=%ds received=%u decoded=%u failed=%u\n",
                   tick, st.received, st.decoded, st.decodeFailed);
      std::fflush(stderr);
    }
  }

  const Stats st = engine->GetStats();
  std::fprintf(stderr, "=== rdc_cli_viewer ===\n");
  std::fprintf(stderr, "connected : %s\n", connected ? "YES" : "NO");
  std::fprintf(stderr, "received  : %u\n", st.received);
  std::fprintf(stderr, "decoded   : %u\n", st.decoded);
  std::fprintf(stderr, "decFailed : %u\n", st.decodeFailed);
  std::fprintf(stderr, "hostSize  : %dx%d\n", st.encodeWidth, st.encodeHeight);
  std::fflush(stderr);
  engine->Close();
  return 0;
}
