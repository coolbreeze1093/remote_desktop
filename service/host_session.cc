// rd_host_service — 服务内常驻被控会话的实现，见 host_session.h。
//
// 锁序（防死锁）：g.mu（引擎/会话） → g_shmMu（共享内存读端） / g_hlpMu
// （helper 管道） / g_sinkMu（事件广播）。各子系统的处理线程只拿自己那把
// 锁，跨拿必按此顺序。
//
// 会话切换行为：helper 消亡（控制管道断）时引擎不动、主控端不断——画面
// 冻结数秒，新 helper 控制管道接上后若会话活着立即重发 startCapture，帧
// 从新共享内存续上。这与旧架构（helper 死 = WebRTC 死 = 斑马线重连）的
// 本质区别，也是本次重构的目的。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "host_session.h"

#include <windows.h>
#include <sddl.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "debug_log.h"
#include "frame_ring.h"
#include "pipe_proto.h"
#include "rdc/engine.h"

namespace rdhost {

namespace {

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

// ---- 引擎/会话状态（g.mu 保护）------------------------------------------

struct HostState {
  std::mutex mu;
  std::unique_ptr<rdc::Engine> engine;
  bool hostActive = false;
  bool stopRequested = false;
  long long captureScreenId = 0;  // 当前会话采集的屏（helper 重连后恢复用）
  long long defaultScreenId = 0;  // 面板推送的缺省屏（startHost 无 screen 时）
  std::vector<std::string> stunServers;
};

HostState g;
std::atomic<bool> g_running{false};

// ---- 事件广播（信令中继 + 面板管道各一个 sink）--------------------------

std::mutex g_sinkMu;
std::vector<std::function<void(const std::string&)>> g_sinks;

void Broadcast(const std::string& json) {
  std::lock_guard<std::mutex> lk(g_sinkMu);
  for (const auto& sink : g_sinks) {
    sink(json);
  }
}

// ---- helper 控制管道（客户端，服务唯一客户端）---------------------------

std::mutex g_hlpMu;
HANDLE g_helperPipe = INVALID_HANDLE_VALUE;

bool SendToHelper(const std::string& json) {
  std::lock_guard<std::mutex> lk(g_hlpMu);
  if (g_helperPipe == INVALID_HANDLE_VALUE) {
    return false;
  }
  if (!rdpipe::WriteFrame(g_helperPipe, json)) {
    rdDebug("host-session", "helper pipe write failed err=%lu", GetLastError());
    return false;
  }
  return true;
}

bool SendStartCapture(long long screenId) {
  const bool ok = SendToHelper("{\"type\":\"startCapture\",\"screenId\":" +
                               std::to_string(screenId) + "}");
  if (!ok) {
    rdDebug("host-session", "startCapture dropped (helper pipe down)");
  }
  return ok;
}

void SendStopCapture() {
  // 幂等：helper 无采集时无操作；管道断（helper 正在消亡）也无操作，
  // helper 侧管道断开自会停采集。
  SendToHelper("{\"type\":\"stopCapture\"}");
}

// ---- 共享内存读端（帧泵与 captureReady 处理之间移交）--------------------

std::mutex g_shmMu;
std::shared_ptr<rdframe::FrameReader> g_reader;

// ---- helper info 缓存（queryScreens / 面板 hello 的应答源）--------------

std::mutex g_infoMu;
std::string g_helperInfo;

// helper 未上线时的占位（与旧 relay 的默认列表一致）：主控端仍可发 Offer，
// 引擎照常应答，画面等 helper 拉起后续上。
const char kNoHelperInfo[] =
    "{\"type\":\"info\",\"version\":\"helper offline\",\"elevated\":0,"
    "\"screenIds\":[0],\"screenTitles\":[\"host default\"]}";

// ---- 会话拆装（须持 g.mu）------------------------------------------------

void TeardownLocked(const char* why) {
  SendStopCapture();
  {
    std::lock_guard<std::mutex> lk(g_shmMu);
    g_reader.reset();
  }
  if (g.engine) {
    g.engine->Close();
    g.engine.reset();
  }
  g.hostActive = false;
  g.stopRequested = false;
  rdDebug("host-session", "session torn down (%s)", why);
}

// startHost 的工作体：AcceptOfferCreateAnswer 最长阻塞 10s，不能占调用方
// 线程（信令收帧线程/面板管道线程）。排队后由 g.mu 串行化；stopHost 置
// stopRequested 使排队中的 start 作废（与旧 helper 相同的语义）。
void WorkerStartHost(long long screenId, std::string offer) {
  std::lock_guard<std::mutex> lk(g.mu);
  if (g.stopRequested) {
    return;
  }
  if (g.engine) {  // 残留清理（正常路径无）
    g.engine->Close();
    g.engine.reset();
  }
  rdc::Config config;
  config.iceServers = g.stunServers;
  auto engine = rdc::Engine::Create(rdc::Role::Host, config);
  if (!engine) {
    Broadcast("{\"type\":\"error\",\"msg\":\"engine create failed\"}");
    return;
  }
  // 回调都不持有引擎指针：对象在下方才移交 g.engine，期间事件照样到达。
  engine->OnState([](rdc::State s, const std::string& detail) {
    Broadcast(std::string("{\"type\":\"state\",\"state\":\"") + StateName(s) +
              "\",\"detail\":" + rdpipe::JStr(detail) + "}");
    if (s == rdc::State::Closed || s == rdc::State::Failed) {
      // 引擎派发线程里不能同步 Close（可能自等待）：分离线程收尾。
      // hostActive=false（建立中失败）时不重复拆，由 WorkerStartHost 收尾。
      std::thread([] {
        std::lock_guard<std::mutex> lk(g.mu);
        if (g.hostActive) {
          rdDebug("host-session", "peer gone — tearing down host session");
          TeardownLocked("peer gone");
          Broadcast("{\"type\":\"state\",\"state\":\"closed\","
                    "\"detail\":\"peer gone\"}");
        }
      }).detach();
    }
  });
  engine->OnError([](const std::string& error) {
    Broadcast("{\"type\":\"error\",\"msg\":" + rdpipe::JStr(error) + "}");
  });
  engine->OnInput([](const rdc::InputEvent& ev) {
    // 转发 helper 注入；helper 不在（会话切换间隙）时丢弃。
    std::string msg = "{\"type\":\"input\",\"t\":" +
                      std::to_string(static_cast<int>(ev.type)) +
                      ",\"nx\":" + std::to_string(ev.nx) +
                      ",\"ny\":" + std::to_string(ev.ny) +
                      ",\"button\":" + std::to_string(ev.button) +
                      ",\"pressed\":" + std::to_string((int)ev.pressed) +
                      ",\"wheel\":" + std::to_string(ev.wheelDelta) +
                      ",\"key\":" +
                      std::to_string(
                          static_cast<unsigned long long>(ev.keyCode)) +
                      ",\"ext\":" + std::to_string((int)ev.extended) + "}";
    SendToHelper(msg);
  });
  if (!engine->Start()) {
    Broadcast("{\"type\":\"error\",\"msg\":\"engine start failed\"}");
    return;
  }
  const std::string answer = engine->AcceptOfferCreateAnswer(offer);
  if (g.stopRequested) {
    engine->Close();
    return;
  }
  if (answer.empty()) {
    Broadcast("{\"type\":\"error\",\"msg\":\"accept offer failed\"}");
    engine->Close();
    return;
  }
  g.engine = std::move(engine);
  g.captureScreenId = screenId;
  g.hostActive = true;
  Broadcast("{\"type\":\"answer\",\"sdp\":" + rdpipe::JStr(answer) + "}");
  SendStartCapture(screenId);
  // 主控端刚连上：让 helper 立即重发当前光标位置/形态（否则要等下次变化）。
  SendToHelper("{\"type\":\"pollCursor\"}");
  rdDebug("host-session", "host started (screen=%lld)", screenId);
}

// ---- helper 事件 ----------------------------------------------------------

void HandleHelperEvent(const std::string& json) {
  const std::string type = rdpipe::JsonGetString(json, "type");
  if (type == "info") {
    {
      std::lock_guard<std::mutex> lk(g_infoMu);
      g_helperInfo = json;
    }
    rdDebug("host-session", "helper info received");
    // 已连接的面板即时刷新（屏幕列表/就绪状态）：旧架构面板管道挂在
    // helper 上，helper 重启=管道断=面板重连自然刷新；现在管道常驻服务，
    // 必须主动广播才不会留过期列表。
    Broadcast(json);
    return;
  }
  if (type == "secure") {
    const bool secure = rdpipe::JsonGetInt(json, "secure") != 0;
    const bool capturable = rdpipe::JsonGetInt(json, "capturable") != 0;
    {
      std::lock_guard<std::mutex> lk(g.mu);
      if (g.engine) {
        // 送达主控端（旧架构 helper 漏了这一步，服务模式下锁屏提示不显示）
        g.engine->SendSecureDesktopState(secure, capturable);
      }
    }
    Broadcast(json);  // 面板锁屏提示
    return;
  }
  if (type == "cursor") {
    // 光标位置（归一化坐标）转发给主控端（include_cursor=false 后主控端
    // 本地渲染光标）。
    std::lock_guard<std::mutex> lk(g.mu);
    if (g.engine) {
      g.engine->SendCursorPosition(rdpipe::JsonGetDouble(json, "nx", 0),
                                   rdpipe::JsonGetDouble(json, "ny", 0),
                                   rdpipe::JsonGetInt(json, "vis", 0) != 0);
    }
    return;
  }
  if (type == "cursorShape") {
    // 光标形态（预乘 ARGB 位图 base64）：仅形态变化时才会到达，直接转发。
    rdc::CursorShape shape;
    shape.width = static_cast<uint32_t>(rdpipe::JsonGetInt(json, "w", 0));
    shape.height = static_cast<uint32_t>(rdpipe::JsonGetInt(json, "h", 0));
    shape.hotspotX = static_cast<uint32_t>(rdpipe::JsonGetInt(json, "hx", 0));
    shape.hotspotY = static_cast<uint32_t>(rdpipe::JsonGetInt(json, "hy", 0));
    std::vector<uint8_t> bgra;
    if (!rdpipe::Base64Decode(rdpipe::JsonGetString(json, "bgra"), &bgra) ||
        shape.width == 0 || shape.height == 0 || shape.width > 4096 ||
        shape.height > 4096 ||
        bgra.size() != static_cast<size_t>(shape.width) * shape.height * 4) {
      return;
    }
    shape.bgra = std::move(bgra);
    std::lock_guard<std::mutex> lk(g.mu);
    if (g.engine) {
      g.engine->SendCursorShape(shape);
    }
    return;
  }
  if (type == "captureReady") {
    // helper 每次开始采集都建新的唯一名映射（分辨率可能变、旧 helper 的
    // 映射可能还在），读端随之换名重开（lastSeq 清零，见 frame_ring.h）。
    // 映射名全 ASCII（helper 生成），逐字符放宽即可。
    const std::string nameA = rdpipe::JsonGetString(json, "name");
    const std::wstring name(nameA.begin(), nameA.end());
    auto reader = std::make_shared<rdframe::FrameReader>();
    if (reader->Open(name)) {
      std::lock_guard<std::mutex> lk(g_shmMu);
      g_reader = reader;
      rdDebug("host-session", "frame ring attached (%ldx%ld stride=%ld)",
              rdpipe::JsonGetInt(json, "w"), rdpipe::JsonGetInt(json, "h"),
              rdpipe::JsonGetInt(json, "stride"));
    } else {
      rdDebug("host-session", "frame ring open FAILED err=%lu", GetLastError());
    }
    return;
  }
}

// ---- 常驻线程 -------------------------------------------------------------

// 帧泵：共享内存 → 引擎。引擎同步拷贝 ~5-8ms 在 g.mu 内进行，短暂挡住
// StartHost/StopHost 排队可接受；换来的是拷贝途中引擎/读端绝不会被拆。
void FramePumpLoop() {
  while (g_running.load()) {
    bool pushed = false;
    {
      std::lock_guard<std::mutex> lk(g.mu);
      std::shared_ptr<rdframe::FrameReader> reader;
      {
        std::lock_guard<std::mutex> sk(g_shmMu);
        reader = g_reader;
      }
      if (g.hostActive && g.engine && reader && reader->opened()) {
        pushed = reader->ConsumeNewest(
            [&](const uint8_t* px, int w, int h, int stride) {
              g.engine->PushVideoFrameBgra(px, stride, w, h);
            });
      }
    }
    Sleep(pushed ? 2 : 8);
  }
}

void StatsLoop() {
  auto last = std::chrono::steady_clock::now();
  while (g_running.load()) {
    Sleep(200);
    if (!g_running.load()) {
      break;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(1)) {
      continue;
    }
    std::lock_guard<std::mutex> lk(g.mu);
    if (!g.hostActive || !g.engine) {
      continue;
    }
    last = now;
    const rdc::Stats st = g.engine->GetStats();
    Broadcast("{\"type\":\"stats\",\"captured\":" +
              std::to_string(st.captured) +
              ",\"encoded\":" + std::to_string(st.encoded) +
              ",\"sent\":" + std::to_string(st.sent) +
              ",\"dropped\":" + std::to_string(st.dropped) +
              ",\"qualityLevel\":" + std::to_string(st.qualityLevel) +
              ",\"encodeWidth\":" + std::to_string(st.encodeWidth) +
              ",\"encodeHeight\":" + std::to_string(st.encodeHeight) +
              ",\"encodeMs\":" + std::to_string(st.encodeMs) + "}");
  }
}

void HelperPipeLoop() {
  while (g_running.load()) {
    HANDLE pipe =
        CreateFileW(rdpipe::kHelperPipeName, GENERIC_READ | GENERIC_WRITE, 0,
                    nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      // helper 未起/正在重启（会话切换）：安静等待。
      Sleep(1000);
      continue;
    }
    {
      std::lock_guard<std::mutex> lk(g_hlpMu);
      g_helperPipe = pipe;
    }
    rdDebug("host-session", "helper control pipe connected");
    // 新 helper 接入：会话活着就立即恢复采集（解锁/切会话后画面续上）。
    long long resumeScreen = -1;
    {
      std::lock_guard<std::mutex> lk(g.mu);
      if (g.hostActive && g.engine) {
        resumeScreen = g.captureScreenId;
      }
    }
    if (resumeScreen >= 0) {
      rdDebug("host-session", "session active — resuming capture (screen=%lld)",
              resumeScreen);
      SendStartCapture(resumeScreen);
      // 新 helper 的光标缓存是空的，顺带请求一次强制补发。
      SendToHelper("{\"type\":\"pollCursor\"}");
    }
    std::string rx;
    bool up = true;
    while (up && g_running.load()) {
      if (!rdpipe::DrainAvailable(pipe, &rx)) {
        break;  // helper 消亡/重启
      }
      std::string msg;
      while (rdpipe::PopFrame(&rx, &msg)) {
        HandleHelperEvent(msg);
      }
      Sleep(10);  // 输入事件经此下发，轮询间隔决定注入延迟
    }
    {
      std::lock_guard<std::mutex> lk(g_hlpMu);
      g_helperPipe = INVALID_HANDLE_VALUE;
    }
    CloseHandle(pipe);
    if (!g_running.load()) {
      break;
    }
    // helper 没了：引擎与主控端连接原样保留，只丢帧。绝不广播 closed——
    // 那会把重构想要避免的"断开"又带回来。
    rdDebug("host-session", "helper control pipe lost — reconnecting");
    Sleep(500);
  }
}

// ---- 面板命名管道服务端（协议与旧 helper 完全一致，GUI 零改动）-----------

struct PanelClient {
  HANDLE pipe = nullptr;
  std::mutex qmu;
  std::deque<std::string> outbox;  // 待发广播事件（本线程刷出）
};

std::mutex g_clMu;
std::deque<PanelClient*> g_clients;

// 注册为事件 sink：引擎/helper 事件进每个面板客户端的发件箱（各客户端
// 线程自己刷出）。
void PanelBroadcast(const std::string& json) {
  std::lock_guard<std::mutex> lk(g_clMu);
  for (PanelClient* c : g_clients) {
    std::lock_guard<std::mutex> qk(c->qmu);
    c->outbox.push_back(json);
  }
}

// 管道 DACL：允许 SYSTEM、管理员组与交互登录用户连接，其余拒绝。
// 必须含交互用户（IU）：面板以普通权限运行也要连得上（语义沿用旧 helper）。
// 返回值随管道生命周期 LocalFree。
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

std::string MakeInfoForPanel() {
  std::string info;
  {
    std::lock_guard<std::mutex> lk(g_infoMu);
    info = g_helperInfo;
  }
  return info.empty() ? std::string(kNoHelperInfo) : info;
}

void PanelClientThread(PanelClient* c) {
  HANDLE pipe = c->pipe;
  rdpipe::WriteFrame(pipe, MakeInfoForPanel());
  std::string rx;
  bool alive = true;
  while (alive) {
    if (!rdpipe::DrainAvailable(pipe, &rx)) {
      break;  // 客户端断开
    }
    std::string msg;
    while (alive && rdpipe::PopFrame(&rx, &msg)) {
      const std::string type = rdpipe::JsonGetString(msg, "type");
      if (type == "hello") {
        rdpipe::WriteFrame(pipe, MakeInfoForPanel());
      } else if (type == "startHost") {
        const bool hasScreen = !rdpipe::JsonRawValue(msg, "screenId").empty();
        long long screenId;
        {
          std::lock_guard<std::mutex> lk(g.mu);
          screenId = hasScreen ? rdpipe::JsonGetInt(msg, "screenId", 0)
                               : g.defaultScreenId;
        }
        const std::string offer = rdpipe::JsonGetString(msg, "offer");
        rdDebug("host-session",
                "panel startHost (screen=%lld%s offerLen=%lu)", screenId,
                hasScreen ? "" : " default", (unsigned long)offer.size());
        if (offer.empty()) {
          alive = rdpipe::WriteFrame(
              pipe, "{\"type\":\"error\",\"msg\":\"empty offer\"}");
        } else if (!StartHost(screenId, offer)) {
          rdDebug("host-session", "panel startHost rejected: host already active");
          alive = rdpipe::WriteFrame(
              pipe, "{\"type\":\"error\",\"msg\":\"host already active\"}");
        }
      } else if (type == "stopHost") {
        rdDebug("host-session", "panel stopHost");
        StopHost("stopped");
      } else if (type == "setQuality") {
        SetQualityMode(rdpipe::JsonGetInt(msg, "mode", 0));
      } else if (type == "setDefaultScreen") {
        std::lock_guard<std::mutex> lk(g.mu);
        g.defaultScreenId = rdpipe::JsonGetInt(msg, "screenId", 0);
        rdDebug("host-session", "default screen set to %lld",
                g.defaultScreenId);
      }
    }
    // 刷广播事件；写失败即本客户端断开。
    std::deque<std::string> out;
    {
      std::lock_guard<std::mutex> qk(c->qmu);
      out.swap(c->outbox);
    }
    for (const auto& e : out) {
      if (!rdpipe::WriteFrame(pipe, e)) {
        alive = false;
        break;
      }
    }
    Sleep(100);
  }
  FlushFileBuffers(pipe);
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
  {
    std::lock_guard<std::mutex> lk(g_clMu);
    for (auto it = g_clients.begin(); it != g_clients.end(); ++it) {
      if (*it == c) {
        g_clients.erase(it);
        break;
      }
    }
    const size_t left = g_clients.size();
    rdDebug("host-session", "panel client disconnected (left=%lu)",
            (unsigned long)left);
  }
  delete c;
}

void PanelPipeLoop() {
  SECURITY_ATTRIBUTES sa;
  HANDLE sd = MakePipeSecurityAttr(&sa);
  if (!sd) {
    rdDebug("host-session", "panel pipe security build FAILED err=%lu",
            GetLastError());
  }
  for (;;) {
    if (!g_running.load()) {
      if (sd) {
        LocalFree(sd);
      }
      return;
    }
    if (!sd) {
      // 无 DACL 兜底：默认描述符（本机一般用户未必能连），下轮重试构建。
      Sleep(2000);
      sd = MakePipeSecurityAttr(&sa);
      continue;
    }
    HANDLE pipe = CreateNamedPipeW(
        rdpipe::kPipeName, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &sa);
    if (pipe == INVALID_HANDLE_VALUE) {
      rdDebug("host-session", "CreateNamedPipe failed err=%lu", GetLastError());
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
    rdDebug("host-session", "panel client connected");
    auto* c = new PanelClient();
    c->pipe = pipe;
    {
      std::lock_guard<std::mutex> lk(g_clMu);
      g_clients.push_back(c);
    }
    std::thread(PanelClientThread, c).detach();
    // 不停止推流：操作者可关掉面板窗口，远程会话保持。
  }
}

}  // namespace

// ---- 公共 API ------------------------------------------------------------

void StartHostSession(const rdcfg::HostConfig& cfg) {
  if (g_running.exchange(true)) {
    return;
  }
  g.stunServers = cfg.stunServers;
  if (cfg.stunOff) {
    // 引擎在 Create 时读该环境变量决定是否补默认 STUN（见 engine.cc）。
    _putenv("RD_NO_STUN=1");
  }
  AddEventSink(PanelBroadcast);  // 信令 sink 由 relay 侧自行注册
  std::thread(PanelPipeLoop).detach();
  std::thread(HelperPipeLoop).detach();
  std::thread(FramePumpLoop).detach();
  std::thread(StatsLoop).detach();
  rdDebug("host-session", "host session started (stun=%d servers, off=%d)",
          (int)cfg.stunServers.size(), (int)cfg.stunOff);
}

void StopHostSession() {
  if (!g_running.exchange(false)) {
    return;
  }
  StopHost("service stopping");
}

bool StartHost(long long screenId, const std::string& offer) {
  if (offer.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lk(g.mu);
  if (g.stopRequested || g.hostActive) {
    return false;
  }
  g.stopRequested = false;
  std::thread(WorkerStartHost, screenId, offer).detach();
  return true;
}

void StopHost(const char* reason) {
  {
    std::lock_guard<std::mutex> lk(g.mu);
    if (!g.hostActive && !g.engine) {
      return;  // 幂等：无会话无操作
    }
    g.stopRequested = true;
  }
  std::thread([reason] {
    std::lock_guard<std::mutex> lk(g.mu);
    if (g.stopRequested) {
      TeardownLocked(reason);
      Broadcast("{\"type\":\"state\",\"state\":\"closed\",\"detail\":" +
                rdpipe::JStr(reason) + "}");
    }
  }).detach();
}

void SetQualityMode(int mode) {
  std::lock_guard<std::mutex> lk(g.mu);
  if (g.engine) {
    g.engine->SetQualityMode(mode);
  }
}

void SetDefaultScreen(long long screenId) {
  std::lock_guard<std::mutex> lk(g.mu);
  g.defaultScreenId = screenId;
}

std::string CachedHelperInfo() {
  std::lock_guard<std::mutex> lk(g_infoMu);
  return g_helperInfo.empty() ? std::string(kNoHelperInfo) : g_helperInfo;
}

void AddEventSink(std::function<void(const std::string&)> sink) {
  std::lock_guard<std::mutex> lk(g_sinkMu);
  g_sinks.push_back(std::move(sink));
}

}  // namespace rdhost
