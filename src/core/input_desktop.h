// remote_desktop — 输入桌面工具（锁屏/UAC 支持）。
//
// Windows 锁屏或 UAC 弹窗时，接收输入的桌面会从 "Default" 切到安全桌面
// （"Winlogon"）。只有进程能打开该桌面（需要管理员或 SYSTEM 权限），才能
// 在其上采集画面（GDI BitBlt）或注入输入（SendInput）；普通权限进程会被
// 拒绝访问（本机实测 GetLastError=5），表现为锁屏后没画面、无法控制。
//
// 用法约定：
//   - 采集线程 / 注入线程在桌面切换时调用 AttachThisThreadToInputDesktop
//     跟随输入桌面；GUI 线程绝不能调用（SetThreadDesktop 要求线程在当前
//     桌面上没有窗口/钩子）。
#ifndef REMOTE_DESKTOP_CORE_INPUT_DESKTOP_H_
#define REMOTE_DESKTOP_CORE_INPUT_DESKTOP_H_

#include <string>

namespace rd {

// 当前接收输入的桌面名（"Default"/"Winlogon"…）。探测失败返回空串；
// deniedOut 非空时写入"是否因权限不足打不开输入桌面"（未提权进程遇安全
// 桌面的固定特征），调用方借此区分"锁屏且无权限"与"探测失败"。
std::wstring CurrentInputDesktopName(bool* deniedOut = nullptr);

// 是否为安全桌面（锁屏/UAC）。名字为空按"未知"处理，返回 false。
bool IsSecureDesktopName(const std::wstring& name);

enum class AttachResult {
  kOnInputDesktop,  // 线程已在（或刚刚附着到）当前输入桌面
  kProbeFailed,     // 桌面句柄已打开但名字查询失败（保持原状态）
  kOpenDenied,      // 打不开输入桌面：权限不足（未提权时锁屏属此类）
  kOpenFailed,      // 打不开输入桌面：其他错误（如会话断开中）
  kSetFailed,       // SetThreadDesktop 失败（线程在原桌面有窗口/钩子）
};

// 把调用线程附着到"当前接收输入的桌面"。按线程缓存已附着的桌面，名字
// 未变时只多一次探测开销；锁屏/解锁切换时自动跟随。SendInput 只有在线程
// 附着到输入桌面时才会落到该桌面的输入流。
AttachResult AttachThisThreadToInputDesktop(std::wstring* nameOut = nullptr);

// 进程令牌是否为提权后的完整令牌（UAC 批准下的管理员进程；SYSTEM 服务
// 亦为真）。这是"锁屏/UAC 采集与注入能否工作"的决定条件：运行时走
// OpenInputDesktop 跟随输入桌面，能否打开由令牌权限决定。
bool IsProcessElevated();

// 锁屏采集与注入是否可用（即进程是否提权/SYSTEM）。宿主启动时预检一次，
// 用于提前提示用户。注意不能只看"按名字打开 Winlogon"——那只是间接信号，
// 个别系统上提权进程也打不开名字引用，会误报"未提权"。
bool CanOpenSecureDesktop();

}  // namespace rd

#endif  // REMOTE_DESKTOP_CORE_INPUT_DESKTOP_H_
