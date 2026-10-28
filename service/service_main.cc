// rd_host_service — 被控端 Windows 服务（LocalSystem，开机自启）。
//
// 用法（同名 exe，按第一个参数分派）：
//   --service              SCM 入口（仅由服务管理器启动）
//   --helper [--session N] 会话 helper（由服务以 SYSTEM 在控制台会话内拉起，
//                          实现在 host_helper.cc，只做采集+注入+共享内存写）
//   --install [--quiet]    注册服务（需管理员），装完立即启动
//   --uninstall [--quiet]  停止并卸载服务，清掉残留 helper
//
// 服务本体是个监督者：轮询活动控制台会话（WTSGetActiveConsoleSessionId），
// 把自己复制成目标会话内的 SYSTEM 进程跑 helper；会话切换/helper 退出时
// 自动重启。业务都在服务里（host_session.cc）：WebRTC 引擎/编码/信令/面板
// 管道常驻 session 0，helper 消亡（解锁/切会话）只冻结画面几秒，主控端连
// 接不断——新 helper 拉起后经控制管道重发 startCapture 续上。
//
// 为什么采集与注入必须"搬"进控制台会话：服务活在 session 0，看不见也摸不
// 着交互桌面的采集与输入；必须把进程"搬"进控制台会话（复制令牌 +
// SetTokenInformation TokenSessionId + CreateProcessAsUser）。帧回传服务
// 走跨会话共享内存（frame_ring.h），输入经控制管道下发（pipe_proto.h）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <wtsapi32.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/input_desktop.h"  // IsProcessElevated（服务身份落日志）
#include "debug_log.h"
#include "host_config.h"
#include "host_session.h"
#include "pipe_proto.h"
#include "signal_relay.h"

// host_helper.cc 提供：会话内被控端进程入口。
int RunHostHelper();

namespace {

constexpr wchar_t kServiceDisplay[] = L"RemoteDesktop Host Service";
constexpr DWORD kInvalidSession = 0xFFFFFFFFu;
// 信令端口不再是常量：exe 旁 rd_host.ini 的 port= 可配（host_config.h），
// 缺省仍是 48010，与旧面板信令端口一致。

using rdpipe::kServiceName;

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_svc = {};
HANDLE g_stopEvent = nullptr;
HANDLE g_helper = nullptr;   // helper 进程句柄
DWORD g_helperSession = kInvalidSession;

std::wstring ExePath() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return path;
}

// 真实系统版本：GetVersionEx 受清单影响会谎报 6.2，RtlGetVersion 永真。
// 记录 build 号便于对照 SetTokenInformation 这类系统行为差异。
void LogOsVersion() {
  using RtlGetVersionFn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
  const auto rtl = reinterpret_cast<RtlGetVersionFn>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  RTL_OSVERSIONINFOW vi = {};
  vi.dwOSVersionInfoSize = sizeof(vi);
  if (rtl && rtl(&vi) == 0) {
    rdDebug("service", "os %lu.%lu build %lu", (unsigned long)vi.dwMajorVersion,
            (unsigned long)vi.dwMinorVersion, (unsigned long)vi.dwBuildNumber);
  }
}

// SYSTEM 令牌持有的特权默认多为"已授予未启用"：SetTokenInformation
// (TokenSessionId) 需要 SE_TCB_NAME，跨会话 CreateProcessAsUser 需要
// SE_ASSIGNPRIMARYTOKEN_NAME / SE_INCREASE_QUOTA_NAME，不启用会在
// SpawnHelper 报 ACCESS_DENIED。服务入口启用一次，进程令牌终身有效。
void EnableSystemPrivileges() {
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
    rdDebug("service", "OpenProcessToken(priv) failed err=%lu", GetLastError());
    return;
  }
  // SE_*_NAME 宏随 UNICODE 与否是窄/宽串，这里按窄串走 A 版查询。
  const LPCSTR kPrivs[] = {SE_TCB_NAME, SE_ASSIGNPRIMARYTOKEN_NAME,
                           SE_INCREASE_QUOTA_NAME};
  for (LPCSTR name : kPrivs) {
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueA(nullptr, name, &tp.Privileges[0].Luid)) {
      rdDebug("service", "LookupPrivilegeValue(%s) failed err=%lu", name,
              GetLastError());
      continue;
    }
    SetLastError(0);
    const BOOL adjusted = AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr,
                                                nullptr);
    const DWORD adjErr = GetLastError();
    if (!adjusted) {
      rdDebug("service", "AdjustTokenPrivileges(%s) failed err=%lu", name,
              adjErr);
    } else if (adjErr == ERROR_NOT_ALL_ASSIGNED) {
      // 令牌没有该特权——正常只应在服务不是 SYSTEM 身份时出现。
      rdDebug("service", "privilege %s NOT assigned (not SYSTEM?)", name);
    } else {
      rdDebug("service", "privilege %s enabled", name);
    }
  }
  CloseHandle(tok);
}

std::wstring ExeDir() {
  std::wstring p = ExePath();
  const size_t s = p.find_last_of(L'\\');
  return s == std::wstring::npos ? L"." : p.substr(0, s);
}

void ReportStatus(DWORD state, DWORD waitHint = 0) {
  if (!g_statusHandle) {
    return;
  }
  g_svc.dwCurrentState = state;
  g_svc.dwWaitHint = waitHint;
  g_svc.dwCheckPoint++;
  SetServiceStatus(g_statusHandle, &g_svc);
}

// 用指定令牌（须已带正确会话号）在会话内启动 helper。
bool LaunchHelperWithToken(HANDLE primary, DWORD session) {
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.lpDesktop = const_cast<LPWSTR>(L"WinSta0\\Default");
  PROCESS_INFORMATION pi = {};
  std::wstring cmd = L"\"" + ExePath() + L"\" --helper --session " +
                     std::to_wstring(session);
  std::wstring dir = ExeDir();
  const BOOL created = CreateProcessAsUserW(
      primary, nullptr, cmd.data(), nullptr, nullptr, FALSE,
      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, dir.c_str(),
      &si, &pi);
  CloseHandle(primary);
  if (!created) {
    return false;
  }
  CloseHandle(pi.hThread);
  g_helper = pi.hProcess;
  g_helperSession = session;
  rdDebug("service", "helper spawned pid=%lu session=%lu",
          (unsigned long)pi.dwProcessId, (unsigned long)session);
  return true;
}

// SetTokenInformation 被系统拒绝时的兜底：WTSQueryUserToken 拿会话用户
// 令牌，会话号天然正确、无需改令牌。helper 以用户身份运行——远控可用，
// 但安全桌面（锁屏/UAC）打不开，面板 info 会显示"非 SYSTEM（异常）"。
bool SpawnHelperAsUser(DWORD session) {
  HANDLE user = nullptr;
  if (!WTSQueryUserToken(session, &user)) {
    rdDebug("service", "WTSQueryUserToken(session=%lu) failed err=%lu",
            (unsigned long)session, GetLastError());
    return false;
  }
  HANDLE primary = nullptr;
  const BOOL ok = DuplicateTokenEx(user, MAXIMUM_ALLOWED, nullptr,
                                   SecurityAnonymous, TokenPrimary, &primary);
  CloseHandle(user);
  if (!ok) {
    rdDebug("service", "DuplicateTokenEx(user) failed err=%lu", GetLastError());
    return false;
  }
  if (!LaunchHelperWithToken(primary, session)) {
    rdDebug("service",
            "CreateProcessAsUser(user session=%lu) failed err=%lu",
            (unsigned long)session, GetLastError());
    return false;
  }
  rdDebug("service", "helper running as USER — lockscreen/UAC uncontrollable");
  return true;
}

// 以 SYSTEM 令牌在指定会话内启动 helper（复制自身令牌改会话号）。
bool SpawnHelper(DWORD session) {
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_DUPLICATE | TOKEN_QUERY, &tok)) {
    rdDebug("service", "OpenProcessToken failed err=%lu", GetLastError());
    return false;
  }
  // 通行（PsExec 式）写法：MAXIMUM_ALLOWED 复制再改会话号。此前显式
  // 四项掩码在被控端实测 SetTokenInformation 报 err=5——特权已启用、
  // SYSTEM 身份均正常仍复现，与已知可用的写法只剩掩码这一处差异。
  HANDLE primary = nullptr;
  const BOOL ok = DuplicateTokenEx(tok, MAXIMUM_ALLOWED, nullptr,
                                   SecurityAnonymous, TokenPrimary, &primary);
  CloseHandle(tok);
  if (!ok) {
    rdDebug("service", "DuplicateTokenEx failed err=%lu", GetLastError());
    return false;
  }
  if (!SetTokenInformation(primary, TokenSessionId, &session,
                           sizeof(session))) {
    const DWORD err = GetLastError();
    CloseHandle(primary);
    rdDebug("service", "SetTokenInformation(SessionId=%lu) failed err=%lu",
            (unsigned long)session, (unsigned long)err);
    return SpawnHelperAsUser(session);
  }
  return LaunchHelperWithToken(primary, session);
}

void KillHelper() {
  if (g_helper) {
    TerminateProcess(g_helper, 0);
    WaitForSingleObject(g_helper, 3000);
    CloseHandle(g_helper);
    g_helper = nullptr;
    g_helperSession = kInvalidSession;
    rdDebug("service", "helper terminated");
  }
}

// 监督循环：跟随活动控制台会话，helper 掉了（崩溃/会话注销）就重拉。
// 未登录（无活动会话）时等待，登录界面（logon 桌面）本身算活动会话，
// 因此开机到登录前 helper 也在运行——登录画面可控。
// 会话切换防抖只保护"活着的 helper"：RDP 同账户连接/断开时
// WTSGetActiveConsoleSessionId 可能短暂 flicker，连续 3 次（6s）稳定才杀
// helper 切换。helper 已死（典型：解锁瞬间系统拆除旧登录会话，helper 随
// 之消亡、进行中的远程会话被打断）则没有可误杀的对象——立即在当前会话
// 重拉，恢复窗口从防抖 6s+ 压到一个轮询周期（≤2s）。
DWORD WINAPI SupervisorLoop(LPVOID) {
  DWORD pendingSession = kInvalidSession;
  int pendingTicks = 0;
  for (;;) {
    const DWORD wait = WaitForSingleObject(g_stopEvent, 2000);
    if (wait == WAIT_OBJECT_0) {
      return 0;
    }
    const DWORD session = WTSGetActiveConsoleSessionId();
    BOOL helperAlive = FALSE;
    if (g_helper) {
      helperAlive = WaitForSingleObject(g_helper, 0) == WAIT_TIMEOUT;
      if (!helperAlive) {
        DWORD code = 0;
        GetExitCodeProcess(g_helper, &code);
        rdDebug("service", "helper exited (session=%lu code=%lu)",
                (unsigned long)g_helperSession, (unsigned long)code);
        CloseHandle(g_helper);
        g_helper = nullptr;
        g_helperSession = kInvalidSession;
      }
    }
    if (session == g_helperSession) {
      pendingSession = kInvalidSession;
      pendingTicks = 0;
      if (!helperAlive && session != kInvalidSession) {
        SpawnHelper(session);  // 崩溃重启
      }
      continue;
    }
    if (g_helper == nullptr) {
      // helper 不在而会话号有变（含刚消亡后控制台已切到新会话）：立即跟进，
      // 不经过防抖；拉起失败下轮再试，日志按会话号去重防刷屏。
      if (session != kInvalidSession) {
        if (session != pendingSession) {
          rdDebug("service", "session switch %lu->%lu (helper down — immediate)",
                  (unsigned long)g_helperSession, (unsigned long)session);
        }
        SpawnHelper(session);
      }
      pendingSession = session;
      pendingTicks = 0;
      continue;
    }
    // 活 helper + 会话号不同：防抖，连续 3 次（6s）稳定才切换。
    if (session == pendingSession) {
      ++pendingTicks;
      if (pendingTicks >= 3) {
        rdDebug("service", "session switch confirmed %lu->%lu",
                (unsigned long)g_helperSession, (unsigned long)session);
        KillHelper();
        if (session != kInvalidSession) {
          SpawnHelper(session);
        }
        pendingSession = kInvalidSession;
        pendingTicks = 0;
      }
    } else {
      pendingSession = session;
      pendingTicks = 1;
    }
  }
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
  g_svc.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_svc.dwControlsAccepted = SERVICE_ACCEPT_STOP;
  g_svc.dwWin32ExitCode = 0;
  g_statusHandle = RegisterServiceCtrlHandlerW(
      kServiceName, [](DWORD ctrl) {
        if (ctrl == SERVICE_CONTROL_STOP) {
          SetEvent(g_stopEvent);
        }
      });
  if (!g_statusHandle) {
    return;
  }
  ReportStatus(SERVICE_START_PENDING, 3000);

  LogOsVersion();
  EnableSystemPrivileges();
  // 业务本体在服务内常驻（引擎/编码/面板管道，host_session），信令监听
  // 由服务承载——两者都在 session 0，不随会话切换消亡。
  // 端口与 STUN 来自 rd_host.ini（port=/stun=），缺省 48010；改动后需重启
  // 服务生效。
  const rdcfg::HostConfig cfg = rdcfg::LoadHostConfig();
  rdhost::StartHostSession(cfg);
  rdrelay::StartSignalRelay(cfg.port);
  g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE thread = CreateThread(nullptr, 0, SupervisorLoop, nullptr, 0, nullptr);

  ReportStatus(SERVICE_RUNNING);
  rdDebug("service", "service running (elevated=%d)",
          (int)rd::IsProcessElevated());

  if (thread) {
    WaitForSingleObject(thread, INFINITE);  // 循环因 STOP 事件返回
    CloseHandle(thread);
  }
  rdrelay::StopSignalRelay();
  rdhost::StopHostSession();
  KillHelper();
  rdDebug("service", "service stopped");
  ReportStatus(SERVICE_STOPPED);
}

// 卸载时清掉残留 helper（服务体已停，按 exe 名匹配只会命中 helper）。
void KillLeftoverHelpers() {
  const std::wstring self = ExePath();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    return;
  }
  PROCESSENTRY32W pe = {};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, self.substr(self.find_last_of(L'\\') + 1).c_str()) == 0 &&
          pe.th32ProcessID != GetCurrentProcessId()) {
        HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
        if (p) {
          TerminateProcess(p, 0);
          CloseHandle(p);
          rdDebug("service", "leftover helper pid=%lu terminated",
                  (unsigned long)pe.th32ProcessID);
        }
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
}

void Notify(const std::wstring& text, bool quiet) {
  if (!quiet) {
    MessageBoxW(nullptr, text.c_str(), kServiceDisplay,
                MB_OK | MB_ICONINFORMATION);
  }
}

// 防火墙规则：服务进程静默绑定端口不会触发防火墙弹窗，不加规则主控端
// 连接会被静默丢弃。失败只落日志不阻断安装（用户可能自备规则）。
// 删除只按规则名匹配（不带端口/程序），这样端口改过之后旧规则也能清掉；
// 安装时先删再加，重装换端口不会残留同名旧规则。
void AddFirewallRule(unsigned short port) {
  std::wstring cmd = L"netsh advfirewall firewall add rule ";
  cmd += L"name=RdHostServiceSignal dir=in action=allow ";
  cmd += L"program=\"" + ExePath() + L"\" ";
  cmd += L"protocol=TCP localport=" + std::to_wstring(port);
  const int rc = _wsystem(cmd.c_str());
  rdDebug("service", "firewall add port=%u rc=%d", (unsigned)port, rc);
}

void DeleteFirewallRule() {
  std::wstring cmd =
      L"netsh advfirewall firewall delete rule name=RdHostServiceSignal";
  const int rc = _wsystem(cmd.c_str());
  rdDebug("service", "firewall delete rc=%d", rc);
}

int DoInstall(bool quiet) {
  // MinGW 头文件缺 SC_MANAGER_CREATE 声明，等价权限直接用 ALL_ACCESS。
  SC_HANDLE sc = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
  if (!sc) {
    rdDebug("service", "OpenSCManager failed err=%lu", GetLastError());
    Notify(L"打开服务管理器失败（需要管理员权限运行）", quiet);
    return 1;
  }
  const std::wstring cmd = L"\"" + ExePath() + L"\" --service";
  SC_HANDLE svc = CreateServiceW(
      sc, kServiceName, kServiceDisplay, SERVICE_ALL_ACCESS,
      SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
      cmd.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
  if (!svc) {
    const DWORD err = GetLastError();
    if (err == ERROR_SERVICE_EXISTS) {
      svc = OpenServiceW(sc, kServiceName, SERVICE_START | SERVICE_CHANGE_CONFIG);
      Notify(L"服务已存在，已尝试启动。", quiet);
    } else {
      CloseServiceHandle(sc);
      rdDebug("service", "CreateService failed err=%lu", err);
      Notify(L"创建服务失败，错误码 " + std::to_wstring(err), quiet);
      return 1;
    }
  }
  // 崩溃自恢复：失败 5s 后重启，共 3 次。
  SERVICE_FAILURE_ACTIONSW fa = {};
  SC_ACTION actions[3] = {};
  for (auto& a : actions) {
    a.Type = SC_ACTION_RESTART;
    a.Delay = 5000;
  }
  fa.cActions = 3;
  fa.lpsaActions = actions;
  ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

  if (!StartServiceW(svc, 0, nullptr)) {
    const DWORD err = GetLastError();
    if (err != ERROR_SERVICE_ALREADY_RUNNING) {
      rdDebug("service", "StartService failed err=%lu", err);
      Notify(L"服务已安装但启动失败，错误码 " + std::to_wstring(err), quiet);
    } else {
      Notify(L"服务已在运行。", quiet);
    }
  } else {
    Notify(L"被控服务安装并启动完成。\n\n控制面板（远程桌面程序 Host 页）"
           L"现在可以连接服务；锁屏 / UAC / 登录界面均可控。", quiet);
  }
  // 端口来自 rd_host.ini：先删同名旧规则再按当前端口加，重装换端口不残留。
  const rdcfg::HostConfig cfg = rdcfg::LoadHostConfig();
  DeleteFirewallRule();
  AddFirewallRule(cfg.port);
  CloseServiceHandle(svc);
  CloseServiceHandle(sc);
  return 0;
}

int DoUninstall(bool quiet) {
  SC_HANDLE sc = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
  if (!sc) {
    Notify(L"打开服务管理器失败（需要管理员权限运行）", quiet);
    return 1;
  }
  SC_HANDLE svc = OpenServiceW(sc, kServiceName,
                               SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
  if (!svc) {
    CloseServiceHandle(sc);
    Notify(L"服务未安装。", quiet);
    return 0;
  }
  SERVICE_STATUS st = {};
  // MinGW 未声明 ControlServiceW/DeleteServiceW（无字符串参数，A/W 等价）。
  ControlService(svc, SERVICE_CONTROL_STOP, &st);
  for (int i = 0; i < 50 && QueryServiceStatus(svc, &st) &&
                  st.dwCurrentState != SERVICE_STOPPED;
       ++i) {
    Sleep(200);
  }
  const BOOL deleted = DeleteService(svc);
  CloseServiceHandle(svc);
  CloseServiceHandle(sc);
  KillLeftoverHelpers();
  DeleteFirewallRule();
  Notify(deleted ? L"被控服务已卸载。" : L"服务已标记删除（重启后彻底移除）。",
         quiet);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // 采集库的 DXGI 路径要求进程 DPI 感知：不带 manifest 的进程在显示缩放
  // （如 150%）下被虚拟化，Duplicator 返回尺寸与 Output 不一致而初始化失败。
  // GUI 程序由 Qt 插件声明感知；服务/helper 没有 Qt，必须自己来。
  if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
    using SetCtxFn = BOOL(WINAPI*)(HANDLE);
    const auto setCtx = reinterpret_cast<SetCtxFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (!setCtx || !setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
      SetProcessDPIAware();  // MinGW 头声明为无参；旧系统兜底
    }
  }

  std::string mode = argc > 1 ? argv[1] : "";
  bool quiet = false;
  for (int i = 2; i < argc; ++i) {
    if (std::string(argv[i]) == "--quiet") {
      quiet = true;
    }
  }

  if (mode == "--service") {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), ServiceMain},
        {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
      rdDebug("service", "not started by SCM (err=%lu) — run with --install",
              GetLastError());
      return 1;
    }
    return 0;
  }
  if (mode == "--helper") {
    return RunHostHelper();
  }
  if (mode == "--install") {
    return DoInstall(quiet);
  }
  if (mode == "--uninstall") {
    return DoUninstall(quiet);
  }
  std::fprintf(stderr,
               "rd_host_service — 被控端服务\n"
               "用法:\n"
               "  --install [--quiet]    安装并启动服务（需管理员）\n"
               "  --uninstall [--quiet]  停止并卸载服务\n"
               "  --service              SCM 入口（勿手动运行）\n"
               "  --helper               会话 helper（由服务拉起）\n");
  return mode.empty() ? 0 : 1;
}