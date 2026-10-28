// rd_host_service --helper — 被控端会话 helper（瘦身后）。
//
// 由服务以 SYSTEM 身份拉起、运行在活动控制台会话内（这是能打开安全桌面的
// 关键：SYSTEM + 交互会话，缺一不可）。引擎上收进服务后，helper 只剩三件事：
//   采集（锁屏跟随） -> 帧写跨会话共享内存（frame_ring.h），
//   收服务下发的输入 -> SendInput 注入，
//   锁屏状态/屏幕信息上报。
// 生命周期由服务管理（被 TerminateProcess 结束即随会话注销，无需善后）。
// 解锁/切会话时本进程随旧会话消亡，WebRTC 会话在服务里毫发无损——服务重
// 拉本进程后重发 startCapture 即续上画面。
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
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <screen_capture/capture.hpp>

#include "core/input_desktop.h"
#include "core/input_injector.h"
#include "core/cursor_monitor.h"
#include "debug_log.h"
#include "frame_ring.h"
#include "pipe_proto.h"
#include "rdc/engine.h"  // InputEvent 定义
#include "session/screen_capture_thread.h"

namespace {

constexpr int kCaptureFps = 24;

// helper 状态：控制管道服务端 + 采集槽 + 注入器 + 共享内存写端。
struct HelperCtx {
  std::mutex pipeMu;
  HANDLE pipe = INVALID_HANDLE_VALUE;  // 到服务的控制管道（本进程是服务端）

  rd::InputInjector injector;  // 几何来自采集回调，注入在控制线程

  std::mutex capMu;  // 采集启停互斥
  std::unique_ptr<rd::ScreenCaptureThread> capture;
  class CaptureSink* sink = nullptr;
  rdframe::FrameWriter writer;
  long long activeScreenId = 0;
};

HelperCtx g;

bool SendToService(const std::string& json) {
  std::lock_guard<std::mutex> lk(g.pipeMu);
  if (g.pipe == INVALID_HANDLE_VALUE) {
    return false;
  }
  return rdpipe::WriteFrame(g.pipe, json);
}

// 光标轮询（include_cursor=false 后光标不经视频，采集线程每帧 + 注入后
// 即时各 Poll 一次，经控制管道 JSON 送服务，服务转控制通道给主控端）。
rd::CursorMonitor g_cursorMonitor;

void SendCursorPos(double nx, double ny, bool visible) {
  SendToService("{\"type\":\"cursor\",\"nx\":" + std::to_string(nx) +
                ",\"ny\":" + std::to_string(ny) +
                ",\"vis\":" + std::to_string((int)visible) + "}");
}

void SendCursorShape(const rd::CursorShapeData& shape) {
  SendToService("{\"type\":\"cursorShape\",\"w\":" +
                std::to_string(shape.width) + ",\"h\":" +
                std::to_string(shape.height) + ",\"hx\":" +
                std::to_string(shape.hotspotX) + ",\"hy\":" +
                std::to_string(shape.hotspotY) + ",\"bgra\":" +
                rdpipe::JStr(rdpipe::Base64Encode(
                    shape.bgra.data(), shape.bgra.size())) +
                "}");
}

// 采集槽：帧 -> 共享内存；几何 -> 注入器；锁屏状态 -> 服务（服务再送达
// 面板与主控端）。首帧时按实际几何建唯一名映射并通知服务挂读端；采集中
// 途几何变化（改分辨率）按同一路径重建，槽尺寸永远与帧严格一致。
class CaptureSink : public rd::FrameSink {
 public:
  void onGeometry(int ox, int oy, int w, int h) override {
    g.injector.setFrameOrigin(ox, oy);
    g.injector.setFrameSize(w, h);
    g_cursorMonitor.setFrameGeometry(ox, oy, w, h);
  }

  void onFrame(const scl_frame& raw) override {
    const int32_t w = static_cast<int32_t>(raw.width);
    const int32_t h = static_cast<int32_t>(raw.height);
    const int32_t stride = static_cast<int32_t>(raw.stride);
    if (!g.writer.opened() || g.writer.width() != w ||
        g.writer.height() != h || g.writer.stride() != stride) {
      if (!EnsureRing(w, h, stride)) {
        return;  // 建映射失败：丢帧（已限频落日志），成功后自动续上
      }
    }
    g.writer.Write(raw.data);
  }

  void onSecureDesktopChanged(bool secure, bool capturable) override {
    rdDebug("helper", "secure desktop changed secure=%d capturable=%d",
            (int)secure, (int)capturable);
    SendToService("{\"type\":\"secure\",\"secure\":" + std::to_string((int)secure) +
                  ",\"capturable\":" + std::to_string((int)capturable) + "}");
  }

 private:
  static std::atomic<int> g_ringSeq;

  bool EnsureRing(int32_t w, int32_t h, int32_t stride) {
    const std::wstring name =
        std::wstring(rdframe::kRingNamePrefix) + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(g_ringSeq.fetch_add(1));
    if (!g.writer.Open(name, w, h, stride)) {
      static std::atomic<int> openDbg{0};
      if (openDbg.fetch_add(1) < 3) {
        rdDebug("helper", "frame ring create FAILED err=%lu "
                          "(非 SYSTEM 兜底令牌建不了 Global\\ 映射?)",
                GetLastError());
      }
      return false;
    }
    rdDebug("helper", "frame ring ready %ls (%ldx%ld stride=%ld)",
            g.writer.name().c_str(), (long)w, (long)h, (long)stride);
    SendToService("{\"type\":\"captureReady\",\"name\":" +
                  rdpipe::JStr(ToAscii(name)) + ",\"w\":" + std::to_string(w) +
                  ",\"h\":" + std::to_string(h) + ",\"stride\":" +
                  std::to_string(stride) + "}");
    return true;
  }

  static std::string ToAscii(const std::wstring& w) {
    return {w.begin(), w.end()};  // 映射名全 ASCII
  }
};
std::atomic<int> CaptureSink::g_ringSeq{0};

void StopCaptureLocked() {
  if (g.capture) {
    g.capture->stop();
    g.capture.reset();
  }
  delete g.sink;
  g.sink = nullptr;
  g.writer.Close();
  g.activeScreenId = 0;
}

void StartCapture(long long screenId) {
  std::lock_guard<std::mutex> lk(g.capMu);
  StopCaptureLocked();
  // SCL_SCREEN_DEFAULT(0) 在个别环境（实测 VMware 虚拟机）会让 DXGI/GDI
  // 采集全部 temporary error：防御性落到第一个真实屏幕；-1（全部）不动。
  long long sid = screenId;
  if (sid == 0) {
    const auto sources = scl::EnumSources(SCL_SOURCE_SCREEN);
    if (!sources.empty()) {
      rdDebug("helper", "screen 0 (default) -> first source %lld (%s)",
              (long long)sources.front().id, sources.front().title.c_str());
      sid = sources.front().id;
    }
  }
  g.sink = new CaptureSink();
  g.capture = std::make_unique<rd::ScreenCaptureThread>();
  if (!g.capture->start(sid, kCaptureFps, g.sink, &g_cursorMonitor)) {
    rdDebug("helper", "capture start FAILED (screen=%lld)", sid);
    StopCaptureLocked();
    return;
  }
  g.activeScreenId = sid;
  rdDebug("helper", "capture started (screen=%lld)", sid);
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

// Global\ 命名空间创建映射需要 SeCreateGlobalPrivilege；SYSTEM 服务令牌
// 默认持有但未必启用，显式启用一次。
void EnableCreateGlobalPrivilege() {
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
    return;
  }
  TOKEN_PRIVILEGES tp = {};
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (LookupPrivilegeValueA(nullptr, SE_CREATE_GLOBAL_NAME,
                            &tp.Privileges[0].Luid)) {
    AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
  }
  CloseHandle(tok);
}

// ---- helper 退出诊断钩子（沿用：会话清理杀进程不留痕迹，靠这里定位）----
BOOL WINAPI HelperCtrlHandler(DWORD ctrl) {
  const char* name = "?";
  switch (ctrl) {
    case CTRL_C_EVENT: name = "CTRL_C"; break;
    case CTRL_CLOSE_EVENT: name = "CTRL_CLOSE"; break;
    case CTRL_LOGOFF_EVENT: name = "CTRL_LOGOFF"; break;
    case CTRL_SHUTDOWN_EVENT: name = "CTRL_SHUTDOWN"; break;
  }
  fprintf(stderr, "[helper] CTRL handler: %s (0x%lx)\n", name, ctrl);
  fflush(stderr);
  return TRUE;
}
void HelperTerminate() {
  rdDebug("helper", "std::terminate called (unhandled C++ exception)");
  fprintf(stderr,
          "[helper] std::terminate called (unhandled C++ exception)\n");
  fflush(stderr);
  std::abort();
}
LONG WINAPI HelperCrashFilter(EXCEPTION_POINTERS* info) {
  if (info && info->ExceptionRecord) {
    rdDebug("helper", "CRASH code=0x%08lx addr=%p threadId=%lu",
            (unsigned long)info->ExceptionRecord->ExceptionCode,
            info->ExceptionRecord->ExceptionAddress,
            (unsigned long)GetCurrentThreadId());
  } else {
    rdDebug("helper", "CRASH (no exception record)");
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
// ----------------------------------------------------------------

// 服务控制管道服务端：唯一客户端是服务本体（SYSTEM），一条连接服务到进
// 程退出。DACL 放行 SYSTEM 与管理员组；命名管道在 \\.\pipe\ 全局命名空间，
// 无需特权——用户令牌兜底模式下 helper 建管道同样可行（只有 Global\ 共享
// 内存需要特权）。管道断开即停采集并重建实例等服务重连（会话切换期间不
// 空转；服务侧重连后会重发 startCapture）。
void ControlServerLoop() {
  SECURITY_ATTRIBUTES sa;
  HANDLE sd = nullptr;
  for (;;) {
    if (!sd) {
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
              "D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &sd, nullptr)) {
        rdDebug("helper", "ctl pipe security build FAILED err=%lu",
                GetLastError());
        Sleep(2000);
        continue;
      }
      sa.nLength = sizeof(sa);
      sa.bInheritHandle = FALSE;
      sa.lpSecurityDescriptor = sd;
    }
    HANDLE pipe = CreateNamedPipeW(
        rdpipe::kHelperPipeName, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &sa);
    if (pipe == INVALID_HANDLE_VALUE) {
      rdDebug("helper", "ctl CreateNamedPipe failed err=%lu", GetLastError());
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
    {
      std::lock_guard<std::mutex> lk(g.pipeMu);
      g.pipe = pipe;
    }
    rdDebug("helper", "control pipe connected (service)");
    SendToService(MakeInfoJson());  // 服务缓存屏幕列表（queryScreens 数据源）

    std::string rx;
    while (true) {
      if (!rdpipe::DrainAvailable(pipe, &rx)) {
        break;  // 服务侧管道断开（服务停止/helper 被切换出活动会话）
      }
      std::string msg;
      while (rdpipe::PopFrame(&rx, &msg)) {
        const std::string type = rdpipe::JsonGetString(msg, "type");
        if (type == "startCapture") {
          StartCapture(rdpipe::JsonGetInt(msg, "screenId", 0));
        } else if (type == "stopCapture") {
          rdDebug("helper", "stopCapture requested");
          std::lock_guard<std::mutex> lk(g.capMu);
          StopCaptureLocked();
        } else if (type == "input") {
          rd::InputEvent ev;
          ev.type = static_cast<rd::InputEvent::Type>(
              rdpipe::JsonGetInt(msg, "t", 0));
          ev.nx = rdpipe::JsonGetDouble(msg, "nx", 0);
          ev.ny = rdpipe::JsonGetDouble(msg, "ny", 0);
          ev.button = (int)rdpipe::JsonGetInt(msg, "button", 0);
          ev.pressed = rdpipe::JsonGetInt(msg, "pressed", 0) != 0;
          ev.wheelDelta = (int)rdpipe::JsonGetInt(msg, "wheel", 0);
          ev.keyCode = (uint32_t)rdpipe::JsonGetInt(msg, "key", 0);
          ev.extended = rdpipe::JsonGetInt(msg, "ext", 0) != 0;
          g.injector.inject(ev);
          if (ev.type == rd::InputEvent::Type::MouseMove ||
              ev.type == rd::InputEvent::Type::MouseButton) {
            // 注入后立即回报光标位置：主控端本地光标不等下一采集帧。
            g_cursorMonitor.Poll();
          }
        } else if (type == "pollCursor") {
          // 会话刚建立：强制重发当前光标位置/形态，主控端不必等下次变化。
          g_cursorMonitor.Poll(true);
        }
      }
      Sleep(5);  // 输入注入延迟敏感
    }
    {
      std::lock_guard<std::mutex> lk(g.pipeMu);
      g.pipe = INVALID_HANDLE_VALUE;
    }
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    rdDebug("helper", "control pipe lost — capture stopped, waiting service");
    {
      std::lock_guard<std::mutex> lk(g.capMu);
      StopCaptureLocked();
    }
  }
}

}  // namespace

// 供 service_main.cc 的 --helper 分支调用。
int RunHostHelper() {
  SetConsoleCtrlHandler(HelperCtrlHandler, TRUE);
  SetUnhandledExceptionFilter(HelperCrashFilter);
  std::set_terminate(HelperTerminate);

  rdDebug("helper", "helper starting (elevated=%d session=%lu)",
          (int)rd::IsProcessElevated(),
          (unsigned long)WTSGetActiveConsoleSessionId());

  EnableCreateGlobalPrivilege();
  g_cursorMonitor.setCallbacks(
      [](double nx, double ny, bool visible) { SendCursorPos(nx, ny, visible); },
      [](const rd::CursorShapeData& shape) { SendCursorShape(shape); });
  std::thread(ControlServerLoop).detach();

  // 主线程无事可做（全部工作在采集线程与控制线程），挂起等待进程被结束。
  for (;;) {
    Sleep(INFINITE);
  }
  return 0;
}
