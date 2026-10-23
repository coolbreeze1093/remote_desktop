// rd_host_service --helper — 被控端会话 helper。
//
// 由服务以 SYSTEM 身份拉起、运行在活动控制台会话内（这是能打开安全桌面的
// 关键：SYSTEM + 交互会话，缺一不可）。职责与 GUI 的 Host 模式一致：
//   采集（锁屏跟随） -> 引擎编码/发送，收对端输入 -> SendInput 注入，
// 另挂一个命名管道服务端接收控制面板的指令、上报状态/Answer/统计。
// 生命周期由服务管理（被 TerminateProcess 结束即随会话注销，无需善后）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <wtsapi32.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <screen_capture/capture.hpp>

#include "core/input_desktop.h"
#include "core/input_injector.h"
#include "debug_log.h"
#include "pipe_proto.h"
#include "rdc/engine.h"
#include "session/screen_capture_thread.h"

namespace {

constexpr int kCaptureFps = 24;

struct Helper;

// 采集槽：帧 -> 引擎；几何 -> 注入器；锁屏状态 -> 面板事件（对齐 GUI 的
// CaptureSink 语义：PushVideoFrameBgra 同步拷贝，返回后调用方即可释放帧）。
// 生命周期归 Helper 所有，必须比 ScreenCaptureThread 活得久。方法实现放在
// Helper 定义之后（需要完整类型）。
class HelperSink : public rd::FrameSink {
 public:
  explicit HelperSink(Helper* h) : h_(h) {}

  void onFrame(const scl_frame& raw) override;
  void onGeometry(int ox, int oy, int w, int h) override;
  void onSecureDesktopChanged(bool secure, bool capturable) override;

 private:
  Helper* h_;
};

struct Helper {
  std::mutex evMu;
  std::deque<std::string> events;

  std::mutex hostMu;  // startHost/stopHost/worker 三方互斥
  std::atomic<bool> stopRequested{false};
  std::atomic<bool> hostActive{false};
  std::unique_ptr<rdc::Engine> engine;
  std::unique_ptr<rd::ScreenCaptureThread> capture;
  std::unique_ptr<HelperSink> sink;
  rd::InputInjector injector;

  void Push(std::string json) {
    std::lock_guard<std::mutex> lk(evMu);
    events.push_back(std::move(json));
  }
};

void HelperSink::onFrame(const scl_frame& raw) {
  if (auto* e = h_->engine.get()) {
    e->PushVideoFrameBgra(raw.data, raw.stride, raw.width, raw.height);
  }
}

void HelperSink::onGeometry(int ox, int oy, int w, int h) {
  h_->injector.setFrameOrigin(ox, oy);
  h_->injector.setFrameSize(w, h);
}

void HelperSink::onSecureDesktopChanged(bool secure, bool capturable) {
  rdDebug("helper", "secure desktop changed secure=%d capturable=%d",
          (int)secure, (int)capturable);
  h_->Push("{\"type\":\"secure\",\"secure\":" + std::to_string((int)secure) +
           ",\"capturable\":" + std::to_string((int)capturable) + "}");
}

const char* StateName(rdc::State s) {
  switch (s) {
    case rdc::State::Idle: return "idle";
    case rdc::State::Gathering: return "gathering";
    case rdc::State::Connecting: return "connecting";
    case rdc::State::Connected: return "connected";
    case rdc::State::Failed: return "failed";
    case rdc::State::Closed: return "closed";
  }
  return "?";
}

// 拆掉当前 host 会话（须持 hostMu；sink 必须比 capture 后释放——stop 先 join）。
void StopHostLocked(Helper* h) {
  if (h->capture) {
    h->capture->stop();
    h->capture.reset();
  }
  h->sink.reset();
  if (h->engine) {
    h->engine->Close();
    h->engine.reset();
  }
  h->hostActive = false;
  h->stopRequested = false;
}

// startHost 全程在独立线程执行（AcceptOffer 最长阻塞 10s，不能卡管道循环）。
// 入队后由 hostMu 串行化；stopHost 会置 stopRequested，使排队中的 start 作废。
void StartHostAsync(Helper* h, long long screenId, std::string offer) {
  std::thread([h, screenId, offer = std::move(offer)] {
    std::lock_guard<std::mutex> lk(h->hostMu);
    if (h->stopRequested) {
      return;
    }
    StopHostLocked(h);

    rdc::Config config;
    config.iceServers = {"stun.miwifi.com:3478"};
    h->engine = rdc::Engine::Create(rdc::Role::Host, config);
    if (!h->engine) {
      h->Push("{\"type\":\"error\",\"msg\":\"engine create failed\"}");
      return;
    }
    h->engine->OnState([h](rdc::State s, const std::string& detail) {
      h->Push(std::string("{\"type\":\"state\",\"state\":\"") + StateName(s) +
              "\",\"detail\":" + rdpipe::JStr(detail) + "}");
    });
    h->engine->OnError([h](const std::string& error) {
      h->Push("{\"type\":\"error\",\"msg\":" + rdpipe::JStr(error) + "}");
    });
    h->engine->OnInput([h](const rdc::InputEvent& ev) {
      h->injector.inject(ev);
    });
    if (!h->engine->Start()) {
      h->Push("{\"type\":\"error\",\"msg\":\"engine start failed\"}");
      h->engine.reset();
      return;
    }
    const std::string answer = h->engine->AcceptOfferCreateAnswer(offer);
    if (h->stopRequested) {
      StopHostLocked(h);
      return;
    }
    if (answer.empty()) {
      h->Push("{\"type\":\"error\",\"msg\":\"accept offer failed\"}");
      StopHostLocked(h);
      return;
    }
    h->sink = std::make_unique<HelperSink>(h);
    h->capture = std::make_unique<rd::ScreenCaptureThread>();
    if (!h->capture->start(screenId, kCaptureFps, h->sink.get())) {
      rdDebug("helper", "capture start FAILED (screen=%lld)", screenId);
      h->Push("{\"type\":\"error\",\"msg\":\"capture start failed\"}");
      StopHostLocked(h);
      return;
    }
    h->hostActive = true;
    h->Push("{\"type\":\"answer\",\"sdp\":" + rdpipe::JStr(answer) + "}");
    rdDebug("helper", "host started (screen=%lld)", screenId);
  }).detach();
}

std::string MakeInfoJson() {
  std::string ids = "[";
  std::string titles = "[";
  bool first = true;
  for (const auto& s : scl::EnumSources(SCL_SOURCE_SCREEN)) {
    if (!first) {
      ids += ",";
      titles += ",";
    }
    first = false;
    ids += std::to_string(s.id);
    titles += rdpipe::JStr(s.title);
  }
  ids += "]";
  titles += "]";
  return "{\"type\":\"info\",\"version\":" +
         rdpipe::JStr("build " __DATE__ " " __TIME__) +
         ",\"elevated\":" + std::to_string((int)rd::IsProcessElevated()) +
         ",\"screenIds\":" + ids + ",\"screenTitles\":" + titles + "}";
}

// 管道 DACL：允许 SYSTEM、管理员组与交互登录用户连接，其余拒绝。
// 必须含交互用户（IU）：helper 可能在登录画面（还取不到会话用户令牌）时
// 就建好管道，登录后同一个 helper 继续服务；若只放 SYSTEM/管理员，普通
// 权限的控制面板将永远连不上——表现为"服务装了也检测不出来"。
// 返回值需要随管道生命周期 LocalFree（helper 进程级一次，进程退出即回收）。
HANDLE MakePipeSecurityAttr(SECURITY_ATTRIBUTES* sa) {
  sa->nLength = sizeof(*sa);
  sa->bInheritHandle = FALSE;
  sa->lpSecurityDescriptor = nullptr;

  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
          "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)", SDDL_REVISION_1, &sd,
          nullptr)) {
    return nullptr;
  }
  sa->lpSecurityDescriptor = sd;
  return sd;
}

int RunHelperImpl() {
  rdDebug("helper", "helper starting (elevated=%d session=%lu)",
          (int)rd::IsProcessElevated(),
          (unsigned long)WTSGetActiveConsoleSessionId());

  Helper h;
  SECURITY_ATTRIBUTES sa;
  HANDLE sd = MakePipeSecurityAttr(&sa);
  if (!sd) {
    rdDebug("helper", "pipe security build FAILED err=%lu", GetLastError());
  }

  for (;;) {
    if (!sd) {
      // 无 DACL 兜底：默认描述符（本机一般用户未必能连），下轮重试构建。
      Sleep(2000);
      sd = MakePipeSecurityAttr(&sa);
      continue;
    }
    HANDLE pipe = CreateNamedPipeW(
        rdpipe::kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64 * 1024,
        64 * 1024, 0, &sa);
    if (pipe == INVALID_HANDLE_VALUE) {
      rdDebug("helper", "CreateNamedPipe failed err=%lu", GetLastError());
      Sleep(2000);
      continue;
    }

    const BOOL connected =
        ConnectNamedPipe(pipe, nullptr)
            ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);
    if (!connected) {
      CloseHandle(pipe);
      continue;
    }
    rdDebug("helper", "panel connected");
    rdpipe::WriteFrame(pipe, MakeInfoJson());

    std::string rx;
    auto lastStats = std::chrono::steady_clock::now();
    bool alive = true;
    while (alive) {
      if (!rdpipe::DrainAvailable(pipe, &rx)) {
        break;  // 面板断开
      }
      std::string msg;
      while (rdpipe::PopFrame(&rx, &msg)) {
        const std::string type = rdpipe::JsonGetString(msg, "type");
        if (type == "hello") {
          rdpipe::WriteFrame(pipe, MakeInfoJson());
        } else if (type == "startHost") {
          const long long screenId = rdpipe::JsonGetInt(msg, "screenId", 0);
          const std::string offer = rdpipe::JsonGetString(msg, "offer");
          std::lock_guard<std::mutex> lk(h.hostMu);
          if (h.hostActive) {
            rdpipe::WriteFrame(
                pipe, "{\"type\":\"error\",\"msg\":\"host already active\"}");
          } else if (offer.empty()) {
            rdpipe::WriteFrame(
                pipe, "{\"type\":\"error\",\"msg\":\"empty offer\"}");
          } else {
            StartHostAsync(&h, screenId, offer);
          }
        } else if (type == "stopHost") {
          h.stopRequested = true;
          std::thread([&h] {
            std::lock_guard<std::mutex> lk(h.hostMu);
            StopHostLocked(&h);
            h.Push("{\"type\":\"state\",\"state\":\"closed\","
                   "\"detail\":\"stopped by panel\"}");
          }).detach();
        } else if (type == "setQuality") {
          const long mode = rdpipe::JsonGetInt(msg, "mode", 0);
          std::lock_guard<std::mutex> lk(h.hostMu);
          if (h.engine) {
            h.engine->SetQualityMode((int)mode);
          }
        }
      }

      const auto now = std::chrono::steady_clock::now();
      if (h.hostActive.load() && now - lastStats >= std::chrono::seconds(1)) {
        lastStats = now;
        const rdc::Stats st = h.engine ? h.engine->GetStats() : rdc::Stats{};
        h.Push("{\"type\":\"stats\",\"captured\":" +
               std::to_string(st.captured) +
               ",\"encoded\":" + std::to_string(st.encoded) +
               ",\"sent\":" + std::to_string(st.sent) +
               ",\"dropped\":" + std::to_string(st.dropped) +
               ",\"qualityLevel\":" + std::to_string(st.qualityLevel) +
               ",\"encodeWidth\":" + std::to_string(st.encodeWidth) +
               ",\"encodeHeight\":" + std::to_string(st.encodeHeight) +
               ",\"encodeMs\":" + std::to_string(st.encodeMs) + "}");
      }

      std::deque<std::string> out;
      {
        std::lock_guard<std::mutex> lk(h.evMu);
        out.swap(h.events);
      }
      for (const auto& e : out) {
        if (!rdpipe::WriteFrame(pipe, e)) {
          alive = false;  // 写失败=面板断开
          break;
        }
      }
      Sleep(120);
    }

    rdDebug("helper", "panel disconnected; host session keeps running");
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    // 不停止推流：操作者可关掉面板窗口，远程会话保持。
  }
}

}  // namespace

// 供 service_main.cc 的 --helper 分支调用。
int RunHostHelper() { return RunHelperImpl(); }
