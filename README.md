# remote_desktop — P2P 远程桌面（rdc 引擎 + screen_capture_lib + Qt + libdatachannel + OpenH264）

无信令服务器的点对点远程桌面工具：主控端（Viewer）看到被控端（Host）的屏幕，
并发送鼠标/键盘控制。双方通过**手动复制粘贴 SDP**（Offer/Answer）建立连接，
连接为 DTLS-SRTP 加密的纯 P2P 通道，不经任何第三方。

所有 WebRTC 相关能力（PeerConnection / DataChannel / RTP 打包解包 / H264 编
解码 / 画质阶梯 / 线程模型）封装在纯 C++17 引擎 **rdc**（`engine/`，零 Qt 依赖）
中，对外只暴露一个门面类 `rdc::Engine`：API 收帧/发输入事件，回调吐帧/吐输入
事件。Qt GUI（`src/`）只是引擎的一个宿主；`examples/` 里有完全无 Qt 的 CLI
宿主样例。

## 架构

```
Host（被控端）                                Viewer（主控端）
┌─────────────────────────┐                 ┌──────────────────────────┐
│ ScreenCaptureThread      │                 │ VideoWidget (Qt 显示)     │
│   ↓ scl::Capturer 抓帧   │   H.264/RTP     │   ↑ QImage               │
│ PushVideoFrameBgra ─────▶│  ═══ SRTP ═══▶ │ OnVideoFrame 回调 ────────│
│ InputInjector (SendInput)│                 │ H264Decoder (OpenH264)    │
│ OnInput 回调 → 注入       │◀══ DataChannel ═│ SendInput(鼠标/键盘 JSON→二进制)
└─────────────────────────┘                 └──────────────────────────┘
          ▲                                          ▲
          └──────────── rdc 引擎（纯 C++，无 Qt）────────────┘
  PeerConnection封装 │ H264RtpPacketizer/Depacketizer │ OpenH264 │ 画质阶梯
  DataChannel 控制协议(v2 二进制) │ 有界丢旧帧队列 │ 编码/解码/派发/统计线程
```

- **视频**：BGRA 抓帧 → libyuv 转/缩放 I420 (1280×720) → OpenH264 编码 →
  H264RtpPacketizer 打包 → DTLS-SRTP 加密发送；接收端对称解码。
  采集与编码在两个线程上流水线并行：采集线程只把 BGRA 拷入有界队列，
  专职编码线程负责 转换→编码→发送（P 帧仍严格串行，压缩率不受影响）；
  队列满时丢最旧的帧，编码永远追最新画面，延迟不会累积
- **控制**：DataChannel 上传**二进制控制协议 v2**（8 字节头 + 定长载荷，
  替代旧 JSON）承载输入事件/帧元数据/解码统计/画质选择，Host 端 SendInput
  注入，坐标按比例映射到被控端虚拟桌面
- **编码参数**：24fps、4Mbps、屏幕内容模式（SCREEN_CONTENT_REAL_TIME），
  连接建立即发关键帧

## rdc 引擎（engine/）

`rdc::Engine`（`engine/include/rdc/engine.h`）是唯一对外接口：

| 方向 | API（收进） | 回调（吐出） |
|------|-------------|--------------|
| 视频 | `PushVideoFrameBgra/I420`（Host 推采集帧） | `OnVideoFrame`（Viewer 收解码帧，shared_ptr 零拷贝） |
| 输入 | `SendInput`（Viewer 发鼠标键盘） | `OnInput`（Host 收远端事件去注入） |
| 信令 | `CreateOffer / AcceptOfferCreateAnswer / AcceptAnswer` | `OnState / OnError` |
| 画质 | `SetQualityMode` | `OnQualityChanged` |
| 诊断 | `GetStats` | `OnRemoteStats` |

回调线程契约写在头文件注释里（`OnVideoFrame` 在解码线程串行、其余在引擎派
发线程、所有 API 任意线程可调）。移植映射：`net/peer_session`→`peer_wrapper`，
`net/control_channel`→`control_port`+`control_protocol.h`（二进制协议，可单测：
`rdc_protocol_test`），`net/h264_stream_sender`→`media_sender`，
`core/openh264_codec`→`video_codec`，`session/video_quality`→`quality_ladder`；
libyuv 源码以相同开关编入 `rdc_yuv` 静态库使引擎自包含。

`src/` 内不再有任何引擎功能的重复实现：`core/openh264_codec`、`core/i420_frame`
已删除（`rd_enc_test` 同样链接 rdc 使用引擎封装），日志统一为 `src/debug_log.h`
→ `rdc/log.h` 别名；src 剩余的 `core/input_injector`（Win32 SendInput）、
`session/screen_capture_thread`（采集）是宿主层职责，按设计留在 src。

无 Qt 宿主样例（SDP 走 CWD 下 `rdc_offer.txt`/`rdc_answer.txt` 交换）：

```powershell
# 终端 1（被控端）          # 终端 2（主控端）
build\examples\rdc_cli_host.exe    build\examples\rdc_cli_viewer.exe --dump first.bmp --inject
# 可选: --screen N --duration S   可选: --duration S
```

## 使用方法

1. **主控端**（想控制别人的一方）运行 `rd_viewer.exe`：点「创建 Offer」→
   复制文本框里的 Offer SDP，用任意途径（聊天窗口/文件）发给对方
2. **被控端**（被控制的一方）运行 `rd_host.exe`：粘贴主控端的 Offer →
   点「生成 Answer」→ 把 Answer SDP 发回主控端（点「开始推流」确认推流）
3. **主控端**：粘贴收到的 Answer → 点「粘贴 Answer 并连接」
4. 连接建立（状态栏显示"已连接"）后主控端出现远端画面，鼠标键盘即接管

ICE 打洞默认走 `stun:stun.l.google.com:19302`（可改
`src/net/peer_session.cc`）；同一局域网内通常无需 STUN 也能直连。

## 构建

要求：Windows、CMake ≥ 3.16、**F:\Qt\Qt5.12.12**（Qt 5.12.12 mingw73_64 +
Tools\mingw730_64 工具链；路径不同改 `CMakeLists.txt` 里的 `CMAKE_PREFIX_PATH`）。

依赖已就位：

```
remote_desktop/third_party/
├── libdatachannel/   # 已编出 build/libdatachannel-static.a (USE_MBEDTLS, 无 ws)
├── mbedtls/          # v3.6.4，已开 MBEDTLS_SSL_DTLS_SRTP，装到 install/mbedtls
└── openh264/         # 已编出 libopenh264.a (MinGW, USE_ASM=No)
screen_capture_lib/   # ../screen_capture_lib，build-qtmingw/libscreen_capture.a
```

```powershell
$env:Path = 'F:\Qt\Qt5.12.12\Tools\mingw730_64\bin;' + $env:Path
cd remote_desktop
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=F:\Qt\Qt5.12.12\Tools\mingw730_64\bin\gcc.exe `
  -DCMAKE_CXX_COMPILER=F:\Qt\Qt5.12.12\Tools\mingw730_64\bin\g++.exe `
  -DCMAKE_MAKE_PROGRAM=F:\Qt\Qt5.12.12\Tools\mingw730_64\bin\mingw32-make.exe
cmake --build build
# 产物 build\src\rd_host.exe（服务端）+ rd_viewer.exe（客户端）
# （windeployqt 已自动部署 Qt DLL；rd_host_service.exe 同目录）
```

若需重编依赖（都走同一套 MinGW 工具链）：

```powershell
# mbedtls（注意配置头里 MBEDTLS_SSL_DTLS_SRTP 需打开）
cmake -S third_party/mbedtls -B third_party/mbedtls/build -G "MinGW Makefiles" `
  -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON `
  -DCMAKE_INSTALL_PREFIX=$PWD\third_party\install\mbedtls
cmake --build third_party/mbedtls/build -- -j 8
cmake --install third_party/mbedtls/build

# libdatachannel（只编静态库目标）
cmake -S third_party/libdatachannel -B third_party/libdatachannel/build `
  -G "MinGW Makefiles" -DCMAKE_PREFIX_PATH=$PWD\third_party\install\mbedtls `
  -DUSE_MBEDTLS=ON -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON `
  -DWARNINGS_AS_ERRORS=OFF -DCMAKE_C_FLAGS="-D__USE_MINGW_ANSI_STDIO=1"
cmake --build third_party/libdatachannel/build --target datachannel-static -j 8

# openh264
cd third_party/openh264
D:\msys64\usr\bin\make.exe OS=mingw_nt ARCH=x86_64 CC=<gcc> CXX=<g++> `
  AR=<ar> libraries -j 8   # 需要 POSIX shell，用 msys2 的 make
```

## 已知限制 / 注意事项

- MinGW GCC 7.3 无 C++/WinRT 头，WGC 路径未启用（screen_capture_lib 在此
  工具链下自动走 DXGI/GDI）；MSVC 构建可打开
- 控制端按键目前映射主要 VK（字母/数字/方向/F1-F12/修饰键等），中文输入法
  等复杂场景不透传
- 构建后自动执行 `windeployqt --release`，部署的是 release 版 Qt DLL（已在
  build 目录验证：Qt5Core.dll ≈ 6.4MB 为 release，170MB 的 Qt5Cored.dll 是
  debug 版，不要混用）；MinGW 运行时（libgcc/libstdc++/libwinpthread）已一并部署
- 跨公网使用需网络可达（STUN 已配）；对称 NAT 等打洞失败场景需自备 TURN

## 动态画质调节

编码不再写死 1280×720/4Mbps。画质按采集显示器的原生尺寸分三档：

| 档位 | 分辨率 | 码率 | 帧率 |
|------|--------|------|------|
| 超清 | 原生 | ~24Mbps | 8fps |
| 高清 | 3/4 缩放 | ~12Mbps | 12fps |
| 流畅 | 1/2 缩放 | ~4Mbps | 24fps |

- **自动**（默认，Viewer 页可选）：从超清起步，编码耗时超出该档预算、
  Viewer 上报解码失败（丢包）、或带宽不足时自动降档；链路干净 12 秒后自动
  升回。档位切换有 3 秒宽限期，避免分辨率切换瞬间的解码失败被误判为丢包
- **手动**：Viewer 页「画质」下拉框选择固定档位，经控制通道实时下发到
  Host（DataChannel 打开时自动同步一次）
- 档位切换会重配编码器并强制 IDR，解码端自适应新分辨率；Host/Viewer 页
  的诊断行实时显示当前档位、编码尺寸、码率与编码耗时 EMA
- 单线程 OpenH264 的吞吐约 30MP/s，超清档因此配 8fps（屏幕内容够用；
  鼠标键盘注入不受帧率影响）

## 诊断「连接后没画面」

两端都有秒级轮询的管道计数器，显示在对应页签顶部和视频窗口占位文本里：

- **Host 页**：`捕获 X / 编码 Y / 已发送 Z / 丢弃 W` —— 捕获为 0 是
  screen_capture_lib 采集失败（看日志 `capturer`/`capture` 行）；编码/发送为 0
  通常意味着还没连接；丢弃持续增长说明编码跟不上采集 24fps（画质阶梯应会
  自动降档；若稳定不降可看日志 `quality`/`encode` 行）
- **Viewer 页 / 视频窗口**：`收到 X 帧 / 解码 Y / 解码失败 Z` —— 收到为 0 说明
  RTP 没到达（网络丢包，连接已建立）；解码失败持续增长说明码流异常

每次运行都会把关键事件追加到 **exe 目录下的 `rd_debug.log`**（ICE 状态、
DTLS/track open、抓屏异常、首批帧），双击启动也会记录；排查问题时把两端
这份文件发回来即可。视频链路已做抗丢包加固：每 2 秒一个 IDR + RTCP NACK
重传 + 对端 PLI 请求关键帧。

## 同机测试注意事项

同一台机器跑两个实例时，**画面显示的确实是被采集屏幕的内容**——如果被采集
的屏幕上摆着深色主题的本程序窗口、或全屏查看窗口与被采集屏重合，看到的就
是"接近全黑 + 界面上的计数文字"（后者还会形成递归镜像），**不是故障**：

- Host 页现在有「共享的屏幕」下拉框，同机测试请选与主控端预览所在显示器
  不同的那块屏
- Viewer 页内嵌实时预览（双击可弹全屏窗口；全屏窗口关闭只是隐藏，不断开）
- 设置环境变量 `RD_DUMP_FIRST_FRAME=<png 路径>` 可把 Viewer 解码出的首帧
  存成图片，直接确认采集内容（端到端管线自检）
