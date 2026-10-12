// rdc_cli_host — 无 Qt 引擎 demo：被控端（Host）。
//
// 验证 rdc 引擎可完全脱离 Qt 使用：
//   1. 引擎 Host 启动（编码线程就绪，等待 onTrack）
//   2. 等待 CWD 下 rdc_offer.txt → AcceptOfferCreateAnswer → 写 rdc_answer.txt
//   3. scl::Capturer 直接抓屏 → engine->PushVideoFrameBgra（~24fps）
//   4. OnInput 回调 → InputInjector 注入鼠标键盘
//   5. --duration 秒后打印管道统计退出
//
// 用法: rdc_cli_host [--screen N] [--duration S]
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include <screen_capture/capture.hpp>

#include "core/input_injector.h"
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

bool ReadFileIf(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  while (!content.empty() && (content.back() == '\n' || content.back() == '\r' ||
                              content.back() == ' ' || content.back() == '\t')) {
    content.pop_back();
  }
  if (content.empty()) {
    return false;
  }
  *out = content;
  return true;
}

void WriteFile(const std::string& path, const std::string& content) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << content;
}

bool WaitForFile(const std::string& path, std::string* out, int timeoutMs) {
  for (int waited = 0; waited < timeoutMs; waited += 200) {
    if (ReadFileIf(path, out)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  int64_t screenId = 0;
  int durationSec = 30;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--screen" && i + 1 < argc) {
      screenId = std::atoll(argv[++i]);
    } else if (a == "--duration" && i + 1 < argc) {
      durationSec = std::atoi(argv[++i]);
    }
  }

  auto engine = Engine::Create(Role::Host);
  rd::InputInjector injector;
  std::atomic<int> inputsInjected{0};
  engine->OnInput([&](const InputEvent& ev) {
    if (injector.inject(ev)) {
      inputsInjected.fetch_add(1);
    }
  });
  engine->OnState([](State s, const std::string& detail) {
    std::fprintf(stderr, "[host] state=%s detail=%s\n", StateName(s),
                 detail.c_str());
    std::fflush(stderr);
  });
  engine->OnError([](const std::string& e) {
    std::fprintf(stderr, "[host] error: %s\n", e.c_str());
    std::fflush(stderr);
  });
  if (!engine->Start()) {
    std::fprintf(stderr, "[host] engine start FAILED\n");
    return 1;
  }

  std::fprintf(stderr, "[host] engine ready; waiting for rdc_offer.txt (60s)...\n");
  std::fflush(stderr);
  std::string offer;
  if (!WaitForFile("rdc_offer.txt", &offer, 60000)) {
    std::fprintf(stderr, "[host] no rdc_offer.txt\n");
    return 2;
  }
  const std::string answer = engine->AcceptOfferCreateAnswer(offer);
  if (answer.empty()) {
    std::fprintf(stderr, "[host] accept offer FAILED\n");
    return 2;
  }
  WriteFile("rdc_answer.txt", answer);
  std::fprintf(stderr, "[host] answer written to rdc_answer.txt (%zu chars)\n",
               answer.size());
  std::fflush(stderr);

  // 采集循环：scl 直接抓屏 → 引擎推帧（与 GUI 的 ScreenCaptureThread 等价）。
  scl::Options opt;
  opt.prefer_wgc = false;
  opt.include_cursor = true;
  opt.detect_updated_region = false;
  auto capturer = scl::Capturer::Create(SCL_SOURCE_SCREEN, opt);
  if (!capturer || !capturer->Select(screenId)) {
    std::fprintf(stderr, "[host] capturer create/select FAILED\n");
    return 3;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(durationSec);
  auto next = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < deadline) {
    next += std::chrono::milliseconds(1000 / 24);
    scl::Frame f = capturer->Capture(1000);
    if (f.data) {
      engine->PushVideoFrameBgra(f.data, f.stride, f.width, f.height);
      scl_frame raw{f.width,   f.height,       f.stride,
                    f.data,    f.capture_time_ms, f.top_left_x, f.top_left_y};
      injector.setFrameOrigin(static_cast<int>(raw.top_left_x),
                              static_cast<int>(raw.top_left_y));
      injector.setFrameSize(raw.width, raw.height);
      scl_frame_free(&raw);
      f.data = nullptr;
    }
    std::this_thread::sleep_until(next);
  }

  const Stats st = engine->GetStats();
  std::fprintf(stderr, "=== rdc_cli_host ===\n");
  std::fprintf(stderr, "captured : %u\n", st.captured);
  std::fprintf(stderr, "encoded  : %u\n", st.encoded);
  std::fprintf(stderr, "sent     : %u\n", st.sent);
  std::fprintf(stderr, "dropped  : %u\n", st.dropped);
  std::fprintf(stderr, "inputs   : %d\n", inputsInjected.load());
  std::fprintf(stderr, "quality  : level=%d %dx%d %dms\n", st.qualityLevel,
               st.encodeWidth, st.encodeHeight, st.encodeMs);
  std::fflush(stderr);
  engine->Close();
  return 0;
}
