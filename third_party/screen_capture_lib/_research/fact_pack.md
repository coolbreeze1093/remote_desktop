# screen_capture_lib 事实包（提取依据存档）

本文记录从 webrtc 树（m114 定制分支，`E:\Github\newwebrtc\webrtc\src`）提取
desktop_capture 子集时确认的关键事实。中间产物（依赖闭包清单、脚本）在
`_research/` 目录。

## 源文件构成（vendor 后 336 个文件）

- **desktop_capture .cc × 59**：ninja 实编清单（`out-Release/Windows-x64/obj/modules/desktop_capture/*.ninja`）
  与 `modules/desktop_capture/BUILD.gn` 声明交叉验证一致：
  - 无条件 21 + is_win 28 + RTC_ENABLE_WIN_WGC 4（wgc_capture_session/source、wgc_capturer_win、wgc_desktop_frame）
  - primitives 5（desktop_frame/geometry/region/shared_desktop_frame/shared_memory）
  - desktop_capture_differ_sse2 1
- **运行时 .cc × 27**：`candidate_runtime_symbols.txt` 给出的 23 对"头→同名 .cc"
  （checks/logging/event/random/string_*/time_utils/win32/win/get_activation_factory 等）
  + 补充 metrics.cc、cpu_features.cc、sleep.cc（DxgiDuplicatorController/wgc_capture_session
  链接期需要 `webrtc::SleepMs`）、platform_thread.cc（event_tracer 的
  `rtc::PlatformThread::SpawnJoinable`）
- **abseil .cc × 7**：bad_optional_access、ascii（kPropertyBits 数据表，match.cc 需要它）、
  match、strings/internal/memutil、base/internal/raw_logging、throw_delegate、numeric/int128
- **libyuv .cc × 27**：全部 generic + `_win.cc`（MSVC x64 下 row.h 启用
  `HAS_I444TOARGBROW_SSSE3` 等宏，实现就在 row_win.cc——不能按 `_win` 后缀排除）；
  排除 `_gcc.cc`（GNU 内联汇编）与 NEON/MSA/LSX/LASX/RVV/jpeg 变体

## 构建配置（对齐原 GN 构建）

- 编译器：MSVC 14.29（VS2019）已验证；defines 对齐
  `desktop_capture.ninja` 第 0 行：`RTC_ENABLE_WIN_WGC`、`WEBRTC_WIN`、
  `NOMINMAX`、`WIN32_LEAN_AND_MEAN`、`_WIN32_WINNT=0x0A00`、
  `NTDDI_VERSION=NTDDI_WIN10_NI`、`PSAPI_VERSION=2`、
  `WINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP`、`ABSL_ALLOCATOR_NOTHROW=1`、
  `LIBYUV_DISABLE_NEON/LSX/LASX`、`DYNAMIC_ANNOTATIONS_ENABLED=0` 等
- 链接库：user32 gdi32 ole32 advapi32 winmm ws2_32 d3d11 dxgi shcore dwmapi
- **不需要** windowsapp.lib / runtimeobject.lib：WGC 的 WinRT 调用经
  `rtc_base/win/get_activation_factory.cc` 与 `hstring.cc` 用
  `LoadLibraryExW(L"combase.dll")` 运行时解析（全树 .ninja 零命中验证）
- 依赖闭包 242 文件中**无 gen/ 生成头、无 libc++ 头、无重模块**
  （media/call/audio/video/pc/p2p 零命中）

## 捕获路径接线（m114 实测源码）

- `DesktopCapturer::CreateScreenCapturer`（desktop_capturer.cc:94）：
  `allow_wgc_screen_capturer() && IsWgcSupported(kScreen)` → WGC；
  否则 `CreateRawScreenCapturer`（screen_capturer_win.cc：DirectX 开 → DXGI+GDI
  fallback，否则 GDI），`detect_updated_region` 时包 DifferWrapper
- `DesktopCapturer::CreateWindowCapturer`（desktop_capturer.cc:71）：
  WGC → CroppingWindowCapturer（`allow_cropping_window_capturer`）→
  `CreateRawWindowCapturer`（window_capturer_win.cc：GDI，Win11 且
  `allow_wgc_capturer_fallback` 时包 BlankDetector→WGC fallback）
- `DesktopCaptureOptions::CreateDefault()` 默认 `allow_directx=false`、
  `allow_wgc_*=false`、`disable_effects=true`——封装层按公开 API 语义显式置位
- `DesktopAndCursorComposer` 构造（desktop_and_cursor_composer.cc:169）：
  `(unique_ptr<DesktopCapturer>, options)`，内部 `MouseCursorMonitor::Create(options)`，
  转发 GetSourceList/SelectSource/FocusOnSelectedSource，光标按
  "桌面坐标 − 帧左上角"合成（窗口帧同样适用）

## MSVC 构建踩坑记录

1. `row.h:269` 的 `#if !defined(LIBYUV_DISABLE_X86) && defined(_MSC_VER) &&
   defined(_M_X64) && (!defined(__clang__) || defined(__SSSE3__))` 会在 MSVC x64
   下启用 SSSE3 行函数声明，实现位于 `row_win.cc` —— vendor 脚本因此保留 `_win.cc`
2. `const char* SCL_API fn()` 形式的后置 `__declspec` MSVC 报 C2059，需前置
3. C++ 门面 `unique_ptr<Impl>` 私有成员：MSVC 14.29 在头文件内急切实例化
   `default_delete` 函数体导致 C2027/C2338，改用声明式删除器
   （`struct ImplDeleter { void operator()(Impl*) const; }`，定义在 .cc）
4. DLL 构建：C API 函数级 `SCL_API` 已足够，但 C++ 门面类需要类级导出
   （`class SCL_API Capturer`），否则 demo 链接 LNK2019

## 验证记录（2026-09-29）

- 静态库 19.2 MB + demo 0.23 MB：枚举 2 屏 + 5 窗口（标题 UTF-8 正常）、
  屏幕 `Select(4)` 连续 3 帧 2560×1600@30–54ms、`Select(0)` 1920×1080、
  窗口捕获 2572×1544，BMP 经 Pillow 打开验证
- DLL 构建 + demo（exe 17KB，运行期加载 screen_capture.dll）通过
- `cmake --install`（头文件/lib/三份 LICENSE）通过
- webrtc 树 `git status` 仅显示原有本地修改（BUILD.gn 的 `//libwebrtc` deps，
  与 out-Release 中既有定制产物一致）与 `screen_capture_lib/` 新增，无源码改动
