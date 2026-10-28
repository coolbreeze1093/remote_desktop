#include "signal_relay.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "host_session.h"
#include "pipe_proto.h"
#include "../src/debug_log.h"

namespace rdrelay {

namespace {

constexpr DWORD kAnswerTimeoutMs = 25000;  // 与主控端信令超时（25s）对齐
constexpr int kRecvPollMs = 500;           // recv 超时轮询（用于查截止时间）

// ---- TCP 信令帧（RDC1）---------------------------------------------------
// 与 GUI 的 src/session/direct_signaling.cc 字节级一致：4 字节魔数 "RDC1"
// + uint32 小端长度 + payload，payload 上限 128KB。helper 管道侧是 rdpipe
// 帧，两边都是自家代码，互不影响。
constexpr char kSignalMagic[4] = {'R', 'D', 'C', '1'};
constexpr uint32_t kMaxSignalPayload = 128 * 1024;

void AppendSignalFrame(std::string* buf, const std::string& payload) {
  buf->append(kSignalMagic, sizeof(kSignalMagic));
  const uint32_t len = static_cast<uint32_t>(payload.size());
  for (int i = 0; i < 4; ++i) {
    buf->push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
  }
  buf->append(payload);
}

// 从 buf 头部取一帧。>0 取到；0 不足一帧（数据保留）；<0 流损坏（已清空）。
int PopSignalFrame(std::string* buf, std::string* payload) {
  if (buf->size() < 8) {
    return 0;
  }
  if (std::memcmp(buf->data(), kSignalMagic, sizeof(kSignalMagic)) != 0) {
    buf->clear();
    return -1;
  }
  uint32_t len = 0;
  std::memcpy(&len, buf->data() + 4, 4);
  if (len > kMaxSignalPayload) {
    buf->clear();
    return -1;
  }
  if (buf->size() - 8 < len) {
    return 0;
  }
  payload->assign(buf->data() + 8, len);
  buf->erase(0, 8 + len);
  return 1;
}

// ---- 共享状态 -----------------------------------------------------------

std::atomic<bool> g_running{false};
SOCKET g_listen = INVALID_SOCKET;

std::mutex g_sessMu;              // 保护下面三个
SOCKET g_client = INVALID_SOCKET; // 当前主控端信令连接（同一时刻至多一个）
bool g_offerTaken = false;        // 已收到 Offer（等 Answer 阶段）
bool g_answered = false;          // Answer 已回（推流中）

std::thread g_acceptThread;

// ---- 工具 ---------------------------------------------------------------

// 关闭当前主控端连接并清会话状态；reason 仅落日志。
void CloseClient(const char* reason) {
  SOCKET s = INVALID_SOCKET;
  {
    std::lock_guard<std::mutex> lk(g_sessMu);
    if (g_client == INVALID_SOCKET) {
      return;
    }
    s = g_client;
    g_client = INVALID_SOCKET;
    g_offerTaken = false;
    g_answered = false;
  }
  closesocket(s);
  rdDebug("relay", "signaling client closed (%s)", reason);
}

// 主控端会话收尾：BYE/断连/超时统一走这里。引擎在服务里常驻，StopHost
// 幂等（无会话无操作），helper 死活不影响信令侧语义。
void EndClientSession(bool offerTaken) {
  if (offerTaken) {
    rdhost::StopHost("session end");
  }
  CloseClient("session end");
}

// ---- 引擎事件 → 信令会话收尾 ---------------------------------------------
// 引擎在服务内常驻（host_session），Answer/错误/会话结束经事件 sink 到达，
// 等价旧 helper 事件转发，只是不再跨进程。

void HandleEngineEvent(const std::string& json) {
  const std::string type = rdpipe::JsonGetString(json, "type");
  if (type == "answer") {
    std::lock_guard<std::mutex> lk(g_sessMu);
    if (g_client == INVALID_SOCKET || !g_offerTaken || g_answered) {
      rdDebug("relay", "stray answer dropped (no pending offer)");
      return;
    }
    const std::string sdp = rdpipe::JsonGetString(json, "sdp");
    if (sdp.empty()) {
      rdDebug("relay", "empty answer");
      closesocket(g_client);
      g_client = INVALID_SOCKET;
      g_offerTaken = false;
      return;
    }
    std::string frame;
    AppendSignalFrame(&frame, sdp);
    const int sent =
        send(g_client, frame.data(), static_cast<int>(frame.size()), 0);
    if (sent == SOCKET_ERROR) {
      rdDebug("relay", "answer send failed err=%d", WSAGetLastError());
      closesocket(g_client);
      g_client = INVALID_SOCKET;
      g_offerTaken = false;
      return;
    }
    g_answered = true;
    rdDebug("relay", "answer relayed len=%lu", (unsigned long)sdp.size());
    // TCP 保持存活：主控端开始 P2P，断开时发 BYE 或直接断连。
    return;
  }
  if (type == "error") {
    // 等 Answer 阶段的错误（accept offer failed / engine start failed …）：
    // 立刻断开让主控端快速失败并自动重试；推流中的错误由 closed 状态收尾。
    std::lock_guard<std::mutex> lk(g_sessMu);
    if (g_client != INVALID_SOCKET && g_offerTaken && !g_answered) {
      const std::string msg = rdpipe::JsonGetString(json, "msg");
      rdDebug("relay", "engine error before answer: %s — closing client",
              msg.c_str());
      closesocket(g_client);
      g_client = INVALID_SOCKET;
      g_offerTaken = false;
    }
    return;
  }
  if (type == "state") {
    const std::string state = rdpipe::JsonGetString(json, "state");
    if (state == "closed" || state == "failed") {
      // 推流会话结束：断开信令连接，主控端感知断开并可自动重连。
      std::lock_guard<std::mutex> lk(g_sessMu);
      if (g_client != INVALID_SOCKET) {
        rdDebug("relay", "engine session %s — closing signaling client",
                state.c_str());
        closesocket(g_client);
        g_client = INVALID_SOCKET;
        g_offerTaken = false;
        g_answered = false;
      }
    }
    return;
  }
  // stats/secure/info：面板经自己的管道收（host_session 广播），这里不留痕。
}

// ---- 主控端信令服务线程（Winsock） --------------------------------------

void SignalClientThread(SOCKET sck) {
  // 收 Offer 阶段需要截止时间兜底：SO_RCVTIMEO 轮询。
  DWORD rcvTo = kRecvPollMs;
  setsockopt(sck, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcvTo),
             sizeof(rcvTo));
  const auto start = std::chrono::steady_clock::now();
  std::string rx;
  bool offerTaken = false;
  bool answered = false;

  bool alive = true;
  while (alive && g_running.load()) {
    char tmp[8192];
    const int got = recv(sck, tmp, sizeof(tmp), 0);
    if (got > 0) {
      rx.append(tmp, static_cast<size_t>(got));
    } else if (got == 0) {
      rdDebug("relay", "signaling peer disconnected");
      break;
    } else {
      const int err = WSAGetLastError();
      if (err != WSAETIMEDOUT) {
        rdDebug("relay", "signaling recv error %d", err);
        break;
      }
    }

    std::string payload;
    for (;;) {
      const int pr = PopSignalFrame(&rx, &payload);
      if (pr < 0) {
        rdDebug("relay", "signaling stream bad (protocol mismatch?) — closing");
        alive = false;
        break;
      }
      if (pr == 0) {
        break;  // 不足一帧
      }
      if (!offerTaken) {
        if (payload == "BYE") {
          alive = false;
          break;
        }
        // JSON 控制帧：目前只有选屏查询（裸 SDP 'v=' 开头不会撞上）；其余
        // JSON（含带 screen 的 Offer）落到下方 Offer 解析，不能在这里吞掉。
        if (!payload.empty() && payload[0] == '{' &&
            rdpipe::JsonGetString(payload, "type") == "queryScreens") {
          const std::string info = rdhost::CachedHelperInfo();
          std::string frame;
          AppendSignalFrame(&frame, info);
          const int sent =
              send(sck, frame.data(), static_cast<int>(frame.size()), 0);
          rdDebug("relay", "screens list -> client (rc=%d)", sent);
          continue;
        }
        // Offer：支持 {"sdp":..,"screen":id}（主控端选屏）与裸 SDP（兼容旧端，
        // 无 screen = helper 默认屏）。
        std::string sdp = payload;
        bool hasScreen = false;
        long long screenId = 0;
        if (!payload.empty() && payload[0] == '{') {
          const std::string s = rdpipe::JsonGetString(payload, "sdp");
          if (s.empty()) {
            // JSON 但没有 sdp：未知控制帧，忽略不当作 Offer。
            rdDebug("relay", "unknown JSON frame (no sdp) ignored");
            continue;
          }
          sdp = s;
          hasScreen = !rdpipe::JsonRawValue(payload, "screen").empty();
          if (hasScreen) {
            screenId = rdpipe::JsonGetInt(payload, "screen", 0);
          }
        }
        offerTaken = true;
        {
          std::lock_guard<std::mutex> lk(g_sessMu);
          g_offerTaken = true;
        }
        const std::string screenLog = hasScreen
                                          ? "screen=" + std::to_string(screenId)
                                          : "host default";
        rdDebug("relay", "offer received len=%lu (%s)",
                (unsigned long)sdp.size(), screenLog.c_str());
        // 引擎常驻服务：立即应答，不再像旧架构那样押住 Offer 等 helper。
        // helper 不在线时画面等其拉起后续上（host_session 恢复逻辑）。
        if (!rdhost::StartHost(screenId, sdp)) {
          rdDebug("relay", "startHost rejected (busy/empty) — closing");
          alive = false;
          break;
        }
        continue;
      }
      if (payload == "BYE") {
        rdDebug("relay", "BYE received");
        alive = false;
        break;
      }
      // 其余帧忽略（协议只定义 Offer/BYE 两类上行）。
    }

    // Answer 截止：引擎迟迟不回（内部出错）——快速失败。
    if (offerTaken && !answered) {
      {
        std::lock_guard<std::mutex> lk(g_sessMu);
        answered = g_answered;  // 由事件 sink 在回帧时置位
      }
      if (!answered &&
          std::chrono::steady_clock::now() - start >
              std::chrono::milliseconds(kAnswerTimeoutMs)) {
        rdDebug("relay", "answer timeout — closing");
        alive = false;
        break;
      }
    }
  }

  EndClientSession(offerTaken);
}

void AcceptLoop(unsigned short port) {
  while (g_running.load()) {
    SOCKET s = accept(g_listen, nullptr, nullptr);
    if (s == INVALID_SOCKET) {
      if (g_running.load()) {
        rdDebug("relay", "accept failed err=%d", WSAGetLastError());
        Sleep(500);
      }
      continue;
    }
    {
      std::lock_guard<std::mutex> lk(g_sessMu);
      if (g_client != INVALID_SOCKET) {
        // 占线：只服务一个主控端，其余立即关闭（与旧面板信令语义一致）。
        closesocket(s);
        continue;
      }
      g_client = s;
      g_offerTaken = false;
      g_answered = false;
    }
    rdDebug("relay", "signaling client arrived");
    std::thread(SignalClientThread, s).detach();
  }
}

}  // namespace

void StartSignalRelay(unsigned short port) {
  if (g_running.exchange(true)) {
    return;
  }
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    rdDebug("relay", "WSAStartup failed");
    g_running.store(false);
    return;
  }
  g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (g_listen == INVALID_SOCKET) {
    rdDebug("relay", "socket() failed err=%d", WSAGetLastError());
    g_running.store(false);
    WSACleanup();
    return;
  }
  BOOL reuse = TRUE;
  setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char*>(&reuse), sizeof(reuse));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(g_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
          0 ||
      listen(g_listen, 8) != 0) {
    rdDebug("relay", "bind/listen(%u) failed err=%d", (unsigned)port,
            WSAGetLastError());
    closesocket(g_listen);
    g_listen = INVALID_SOCKET;
    g_running.store(false);
    WSACleanup();
    return;
  }
  rdDebug("relay", "signaling listening on 0.0.0.0:%u", (unsigned)port);
  rdhost::AddEventSink(HandleEngineEvent);
  g_acceptThread = std::thread(AcceptLoop, port);
}

void StopSignalRelay() {
  if (!g_running.exchange(false)) {
    return;
  }
  if (g_listen != INVALID_SOCKET) {
    closesocket(g_listen);  // 唤醒 accept
    g_listen = INVALID_SOCKET;
  }
  CloseClient("service stopping");
  if (g_acceptThread.joinable()) {
    g_acceptThread.join();
  }
  WSACleanup();
  rdDebug("relay", "signaling relay stopped");
}

}  // namespace rdrelay
