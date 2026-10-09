# screen_capture_lib

从 WebRTC 源码中提取的 **屏幕/窗口捕获独立库**。源码取自
`modules/desktop_capture` 及其最小依赖闭包（rtc_base 子集、libyuv、abseil），
用 **CMake 独立构建**，不依赖 GN/ninja 工具链，也不依赖 WebRTC 树的其余部分。

## 平台支持

| 平台 | 捕获路径 | 配置时代码 | 本库验证状态 |
|---|---|---|---|
| Windows (MSVC) | WGC（Win10 1903+）/ DXGI Duplication（GDI 兑底） / GDI | `RTC_ENABLE_WIN_WGC` | ✅ 已验证（静态+DLL、实抓帧） |
| Windows (MinGW) | DXGI Duplication / GDI（WGC 可选，默认关） | `SCL_ENABLE_WGC=OFF` | ✅ 已验证（静态+DLL、实抓帧） |
| Linux (X11) | X11 + XDamage/Xrandr/Xfixes 等扩展 | `WEBRTC_USE_X11` | ⚙️ 源码就绪，需 X11 dev 环境编译验证 |
| Linux (Wayland) | PipeWire 经 XDG portal（可选 `SCL_USE_PIPEWIRE=ON`） | `RTC_LINK_PIPEWIRE` | ⚙️ 源码就绪，需 pipewire/gbm/egl/libdrm/glib 环境 |
| macOS | CoreGraphics / IOSurface / CGWindow | `WEBRTC_MAC` | ⚙️ 源码就绪（Objective-C++），需 macOS SDK 验证 |

三平台的捕获源码均按 `modules/desktop_capture/BUILD.gn` 的平台条件从 WebRTC 树提取；
公开 API（`screen_capture.h` / `capture.hpp`）平台中立，不含任何平台头文件。

- 鼠标光标可合成进帧（`DesktopAndCursorComposer`）
- 多显示器：枚举各屏幕、可抓整个虚拟桌面（`SCL_SCREEN_ALL`）
- 窗口标题以 UTF-8 返回
- 交付形态：静态库或 DLL/so（`-DSCL_BUILD_SHARED=ON`），同一套源码

## 目录结构

```
screen_capture_lib/
├── CMakeLists.txt              # 独立 CMake 构建
├── include/screen_capture/
│   ├── screen_capture.h        # 公开 C API（稳定入口）
│   └── capture.hpp             # C++ 门面（std 类型、持久 Capturer）
├── src/                        # 封装层实现
├── examples/scl_demo.cc        # 演示程序
├── tools/vendor_from_webrtc.py # 从 webrtc 树生成 vendored/ 的脚本
└── vendored/                   # 提取出的 webrtc/libyuv/abseil 源码（生成物）
```

## 构建

要求：CMake ≥ 3.20。

### Windows（已验证）

VS2019+ 或 MinGW-w64：

```powershell
# MSVC：静态库 + 演示程序（WGC 开启）
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release

# MinGW（MSYS2 mingw64）：GCC 编译，默认关闭 WGC（缺 C++/WinRT 头）
cmake -S . -B build-mingw -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw
```

两个工具链都支持 `SCL_BUILD_SHARED=ON`（DLL）。MinGW 下 WGC 路径自动排除
（`SCL_ENABLE_WGC` 默认 OFF），DXGI/GDI 路径完整可用；若你的 MinGW 环境装了
C++/WinRT 头，可 `-DSCL_ENABLE_WGC=ON` 打开。

### Linux（X11 路径）

先装依赖（Debian/Ubuntu 示例）：

```bash
sudo apt install libx11-dev libxcomposite-dev libxdamage-dev libxext-dev \
                 libxfixes-dev libxrender-dev libxrandr-dev libxtst-dev
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Wayland/PipeWire 路径（可选，镜像 WebRTC `rtc_use_pipewire`；需
`libpipewire-0.3-dev libgbm-dev libegl-dev libepoxy-dev libdrm-dev libglib2.0-dev`，
pkg-config 解析，直连方式，非 dlopen 桩）：

```bash
cmake -S . -B build -DSCL_USE_PIPEWIRE=ON
cmake --build build -j
```

### macOS

Xcode 命令行工具（ObjC++ 自动启用）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### 安装

```powershell
cmake --install build --prefix <安装目录>
```

选项：`SCL_BUILD_SHARED`（默认 OFF）、`SCL_BUILD_DEMO`（默认 ON）、
`SCL_STATIC_RUNTIME`（MSVC /MT）、`SCL_USE_PIPEWIRE`（Linux PipeWire 路径）。

## 使用（C API）

```c
#include <screen_capture/screen_capture.h>

scl_options opt;
scl_default_options(&opt);            /* WGC/DXGI 优先、合成鼠标、禁动画 */

/* 枚举：两段式调用 */
int32_t n = 0;
scl_enum_sources(SCL_SOURCE_WINDOW, &opt, NULL, &n);
scl_source_info* infos = malloc(sizeof(scl_source_info) * n);
scl_enum_sources(SCL_SOURCE_WINDOW, &opt, infos, &n);
/* infos[i].id = HWND，infos[i].title = UTF-8 标题 */

/* 抓一帧（BGRA，自包含调用，可跨线程） */
scl_frame f;
scl_result r = scl_capture(SCL_SOURCE_SCREEN, 0, &opt, &f);
if (r == SCL_OK) {
  /* f.width/height/stride/f.data (BGRA top-down)，f.top_left_* 为桌面原点 */
  scl_frame_free(&f);
}
```

## 使用（C++ 门面）

```cpp
#include <screen_capture/capture.hpp>

scl::Options opt;                     // 默认即 WGC+DXGI+光标
auto cap = scl::Capturer::Create(SCL_SOURCE_SCREEN, opt);
for (const auto& s : cap->GetSourceList()) { /* s.id, s.title */ }

cap->Select(scl::kScreenAll);         // 整个虚拟桌面；或某个屏幕 id / HWND
scl::Frame f = cap->Capture(2000);    // 持久捕获器，DXGI/WGC 会话跨帧复用
// 失败时 f.data == nullptr，cap->last_result() 给出 SCL_ERR_TEMPORARY(可重试)
// 或 SCL_ERR_PERMANENT(需重建捕获器)

std::vector<uint8_t> packed = f.CopyPacked();  // 紧凑 BGRA 深拷贝
// f 析构时自动释放缓冲
```

## 选项语义

| 选项 | 默认 | 说明 |
|---|---|---|
| `prefer_wgc` | 1 | 允许 Windows Graphics Capture（内部再校验 Win11/会话支持，失败自动走下一条路径） |
| `wgc_fallback` | 1 | 窗口捕获时 GDI 检出全黑帧后用 WGC 重试（仅 Win11） |
| `prefer_directx` | 1 | 屏幕捕获用 DXGI Duplication，不支持时回退 GDI |
| `disable_effects` | 1 | 抓窗口期间禁用动画/Aero |
| `include_cursor` | 1 | 把鼠标光标合成进帧 |
| `detect_updated_region` | 0 | 计算脏区（用于流式场景，逐帧开销略增） |
| `capture_timeout_ms` | 2000 | 单帧最长等待 |

## 从 webrtc 树重新提取

`vendored/` 是生成物，可用脚本对任意同类 webrtc 检出重新生成：

```powershell
python tools/vendor_from_webrtc.py --src <webrtc 检出的 src 目录>
```

脚本以 `../_research/` 里的依赖闭包清单为种子，按 `#include` 递归闭包补齐，
webrtc 树只读、不会被修改。

## 版权

`vendored/` 内的 WebRTC、libyuv、abseil 源码分别遵循其原始 BSD 许可证
（见 `vendored/LICENSE.webrtc`、`vendored/third_party/*/LICENSE`）。
封装层（`include/`、`src/`、`examples/`、`CMakeLists.txt`）同以 BSD 3-Clause 提供。

## 已知限制

- Windows 路径完整可用；Linux/macOS 源码与构建分支就绪，但需在对应平台上编译验证
  （本库的开发机为 Windows）
- 暂未暴露桌面音频/光标形状回调/窗口区域裁剪（`allow_cropping_window_capturer`）
- WGC 的会话级 API（帧率控制、IsBorderRequired 等）未透出，走 webrtc 默认策略
- Linux PipeWire 路径采用直连 libpipewire（`RTC_LINK_PIPEWIRE`），
  未实现 WebRTC 的 dlopen 桩（需要时可用 `pipewire.sigs` 生成）
