# 设计：纯 C++ WebRTC 引擎（rdc）——对外 API + 回调，收输入事件、吐视频帧

> **实施状态（2026-10）**：P1/P2/P3 已全部落地并通过验证；P4（C ABI）待定。
> 验证结果：`rdc_protocol_test` 全过；`rd_self_test` 连续两次 PASS（连接、80 帧
> 解码、输入注入）；`rd_pair_test` 双进程双端 PASS（host 编 120 发 120，viewer
> 收 118 解 118 失败 0）；CLI demo 全链路通过（host 采集 439/编 145/发 145/
> 注入 15，viewer 收 121 解 121 失败 0，首帧 BMP 落盘有效）。
>
> 实施中的偏离与补充（以代码为准）：
> 1. `quality_ladder` 为单头文件（逻辑同旧 `video_quality.h`）。
> 2. 引擎只启一条派发线程，同时承载 OnInput/OnState/OnError/OnQualityChanged/
>    OnRemoteStats（设计中的"控制线程/事件线程"合一，顺序保证不变）。
> 3. libyuv 以与 screen_capture_lib 完全相同的文件集与开关编入 `rdc_yuv`
>    静态库挂接 rdc —— 引擎自包含，仅链 rdc 的宿主（如 CLI viewer）也能解析
>    转换符号；与 screen_capture 共存时两份目标文件符号集一致，链接器先到先得。
> 4. libdatachannel 在持有内部锁的状态下派发 onTrack，回调内直接
>    `setMediaHandler` 会自锁（shared_mutex EDEADLK）：attach 必须转投引擎
>    派发线程（旧 Qt 代码靠跳 Qt 线程规避，同理）。
> 5. Qt 适配层把 SDP 阻塞调用放工作线程 + QEventLoop 泵事件保 UI 响应；
>    工作线程只跑纯 std::string 的引擎调用，不碰 Qt 对象（mingw 版 Qt 在
>    非 Qt 线程上构造 QString 会走 TLS 析构链，实测引发堆损坏崩溃）。
> 6. 适配器每次 startHost/startViewer 重建 alive_ 守卫（修复旧代码重启会话后
>    回调被旧标志拦掉的问题）。
> 7. GUI/测试目标链接顺序统一 rdc 在 screen_capture 之前。
> 8. src 内重复实现已彻底清理：`core/openh264_codec`、`core/i420_frame` 删除
>    （`rd_enc_test` 迁移为链接 rdc 使用引擎封装），死文件 `repro_yuv*.cc` 删除；
>    日志实现合一 —— 引擎公开 `rdc/log.h`，`src/debug_log.h` 只是别名（同一份
>    exe 旁 rd_debug.log）；src/CMakeLists 不再直连 openh264/libyuv。

> 目标：把本工程所有 WebRTC 相关能力从 Qt 中剥离，做成纯 C++17 静态库 **`rdc`**
> （remote_desktop core engine）。宿主程序（Qt GUI / CLI / 测试程序）只看到
> 一组普通 C++ 接口和回调：**鼠标键盘事件进、视频帧出**，不感知任何 Qt 类型。

---

## 0. 一个必须先说清的决策：不重写 WebRTC 协议栈

"全部用 C++ 实现"的正确落点是 **引擎层 100% 纯 C++、零 Qt 依赖**，而不是从零
实现 ICE / DTLS-SRTP / SCTP：

| 方案 | 评价 |
|---|---|
| 从零实现 WebRTC 协议栈 | ICE(libjuice 级别) + DTLS + SRTP + SCTP 数据通道，数人月工作量，安全/互通性风险极高，无收益 |
| **保留 libdatachannel 作传输底座，上层全部自研封装为纯 C++ 引擎（本方案）** | libdatachannel 本身就是纯 C++ 的 WebRTC 实现（ICE=libjuice、DTLS-SRTP=mbedTLS/OpenSSL、SCTP=usrsctp），被剥离的是 Qt 耦合层 |
| 换 libwebrtc 全家桶 | 数百 MB 依赖、GN/ depot_tools 构建链与现有 MinGW 工具链冲突，且 API 复杂度远超需求 |

结论：**传输层保留 libdatachannel，Qt 层（PeerSession/ControlChannel/RemoteDesktopSession
中的 QObject、信号槽、QJson）全部用纯 C++ 重写**。现有 H264 打包/解包、OpenH264
编解码、画质阶梯等代码本就无 Qt 依赖，直接迁移进引擎。

---

## 1. 角色与数据流向（对齐需求原话）

引擎是全双工、双角色的，同一套 API 按角色对称工作：

```
Host（被控端）                                     Viewer（主控端）
┌────────────────────────────┐                 ┌────────────────────────────┐
│ 宿主程序                     │                 │ 宿主程序                     │
│  采集(scl) ──PushVideoFrame──▶               │   ◀──OnVideoFrame── 渲染     │
│  注入(SendInput) ◀─OnInput──                │   SendInput──▶ 发送鼠标键盘   │
├────────────────────────────┤                 ├────────────────────────────┤
│ rdc 引擎                     │   SRTP 视频流    │ rdc 引擎                     │
│  BGRA→I420→OpenH264 编码     │  ═══════════▶   │  RTP解包→OpenH264 解码       │
│  RTP 打包 / PLI / NACK      │                 │  解包重排 / PLI 请求          │
│  OnInput 收远端输入事件       │  ◀═══════════   │  SendInput 发输入事件         │
│  控制协议(二进制 DataChannel) │   DataChannel   │  控制协议(画质/统计/元数据)    │
└────────────────────────────┘                 └────────────────────────────┘
```

- **Viewer 视角**：`SendInput()` 把鼠标键盘事件**送进**引擎；`OnVideoFrame` 回调把
  解码后的 I420 帧**吐出来**给宿主渲染。
- **Host 视角**：`PushVideoFrame()` 把采集帧送进引擎；`OnInput` 回调把远端
  鼠标键盘事件**吐出来**给宿主注入。

边界原则：**引擎管"传输 + 编解码 + 协议 + 线程"，宿主管"采集 + 注入 + 渲染"**。
引擎不调用任何 Win32 / Qt API（`InputInjector`、`ScreenCaptureThread`、Qt Widget
全部留在宿主层）。

---

## 2. 对外头文件草案（`engine/include/rdc/engine.h`）

```cpp
// rdc — 纯 C++17 远程桌面 WebRTC 引擎。无 Qt、无平台 UI 依赖。
#ifndef RDC_ENGINE_H_
#define RDC_ENGINE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rdc {

enum class Role { Host, Viewer };

// 粗粒度状态机：宿主据此更新 UI / 日志。
enum class State { Idle, Gathering, Connecting, Connected, Failed, Closed };

// ---------- 数据对象 ----------

// I420 视频帧。回调交付的是 shared_ptr<const VideoFrame>，零拷贝由引用计数保证。
struct VideoFrame {
  int width = 0, height = 0;
  const uint8_t* y = nullptr;  int yStride = 0;
  const uint8_t* u = nullptr;  int uStride = 0;
  const uint8_t* v = nullptr;  int vStride = 0;
  uint64_t timestampUs = 0;
};

// 与现有 rd::InputEvent 字段一一对应（去 Qt 化）。
struct InputEvent {
  enum class Type : uint8_t { MouseMove = 0, MouseButton, MouseWheel, Key };
  Type type = Type::MouseMove;
  double nx = 0, ny = 0;      // 归一化坐标 [0..1]，相对远端帧
  int button = 0;             // 1=左 2=右 3=中
  bool pressed = false;
  int wheelDelta = 0;         // 每格 ±120
  uint32_t keyCode = 0;       // Windows VK
  bool extended = false;
  uint64_t timestampUs = 0;   // 引擎收包时刻（Host 端诊断用）
};

struct Config {
  std::vector<std::string> iceServers;  // 默认 stun:stun.l.google.com:19302
  int ladderLevels = 3;                 // 画质档位数（超清/高清/流畅）
};

// 诊断统计：口径与现有 DiagStats 完全一致，UI 轮询用。
struct Stats {
  uint32_t captured = 0, encoded = 0, sent = 0, dropped = 0;      // Host
  uint32_t received = 0, decoded = 0, decodeFailed = 0;           // Viewer
  int qualityLevel = -1;                                          // 0..2
  int encodeWidth = 0, encodeHeight = 0, encodeMs = 0;
};

// ---------- 引擎门面 ----------

class Engine {
 public:
  virtual ~Engine() = default;

  static std::unique_ptr<Engine> Create(Role role, Config cfg = {});

  // ---- 信令（非 trickle，手动交换 SDP；阻塞至 ICE 收集完成）----
  virtual bool Start() = 0;                                        // 建 PC + 通道
  virtual std::string CreateOffer() = 0;                           // Viewer
  virtual std::string AcceptOfferCreateAnswer(const std::string& offerSdp) = 0;  // Host
  virtual bool AcceptAnswer(const std::string& answerSdp) = 0;     // Viewer
  virtual void Close() = 0;

  // ---- 视频：Host 推帧 ----
  // BGRA 进（引擎内做 libyuv 转/缩放 I420）。非阻塞：内部有界队列(~4)，
  // 满时丢最旧帧并计入 dropped，采集线程永不因编码慢而卡住。
  virtual bool PushVideoFrameBgra(const uint8_t* bgra, int strideBytes,
                                  int width, int height) = 0;
  // 已是 I420 时直推，省一次转换。
  virtual bool PushVideoFrameI420(const uint8_t* y, int yStride,
                                  const uint8_t* u, int uStride,
                                  const uint8_t* v, int vStride,
                                  int width, int height) = 0;

  // ---- 视频：Viewer 吐帧（解码线程串行回调）----
  virtual void OnVideoFrame(
      std::function<void(std::shared_ptr<const VideoFrame>)> cb) = 0;

  // ---- 输入 ----
  virtual void SendInput(const InputEvent& ev) = 0;                // Viewer→Host
  virtual void OnInput(std::function<void(const InputEvent&)> cb) = 0;  // Host 收

  // ---- 事件 / 统计 / 画质 ----
  virtual void OnState(std::function<void(State, const std::string& detail)> cb) = 0;
  virtual void OnError(std::function<void(const std::string&)> cb) = 0;
  // Host 端档位变化（重配编码器+强制 IDR 后触发）。
  virtual void OnQualityChanged(
      std::function<void(int level, int w, int h, int fps, int kbps)> cb) = 0;
  // Viewer → Host 的每秒解码统计（Host 用于自动降档），Host 端经回调接收。
  virtual void OnRemoteStats(
      std::function<void(int received, int decoded, int decodeFailed)> cb) = 0;

  virtual void SetQualityMode(int mode) = 0;   // 0=auto 1=超清 2=高清 3=流畅
  virtual Stats GetStats() const = 0;          // 任意线程可调
};

}  // namespace rdc
#endif
```

### 回调线程契约（写进头文件注释，宿主按契约使用）

| 回调 | 触发线程 | 契约 |
|---|---|---|
| `OnVideoFrame` | 引擎解码线程（单线程串行） | 轻活；持有 `shared_ptr` 即可离线程用 |
| `OnInput` | 引擎控制线程 | 顺序与发送方一致；禁阻塞 |
| `OnState` / `OnError` / `OnQualityChanged` | 引擎事件线程（见 §4） | 禁阻塞 |
| 所有 `Engine` 方法 | 任意线程 | 内部加锁，可重入安全 |

回调中**禁止**同步调用 Engine 的信令方法（`CreateOffer` 等会等 ICE，重入死锁）；
引擎内部对这条路径做 assert 防护。

---

## 3. 内部模块划分

```
engine/
├── include/rdc/engine.h          // 上面的门面 + 数据对象
├── engine.cc                     // 门面实现、状态机、事件线程、生命周期守卫
├── signaling_port.{h,cc}         // rtc::PeerConnection 封装（非 trickle、SDP 阻塞收集）
├── media_port.{h,cc}             // rtc::Track 封装：H264RtpPacketizer/Depacketizer、PLI/NACK、open 转换强制 IDR
├── video_codec_port.{h,cc}       // OpenH264 编/解码封装（自 core/openh264_codec 原样迁移）
├── control_port.{h,cc}           // DataChannel：二进制控制协议编解码 + 派发
├── control_protocol.h            // 协议常量、serialize/deserialize（纯函数，可单测）
├── quality_ladder.{h,cc}         // 三档画质 + 自动升降档（自 session/video_quality 迁移）
├── frame_queue.h                 // 有界丢旧帧队列（自 RemoteDesktopSession 内联逻辑抽出）
└── event_thread.h                // 回调派发线程 + alive 守卫
```

各模块职责与现状映射（迁移即"去 Qt 化"，逻辑不变）：

| 现有代码 | 去向 | 改动 |
|---|---|---|
| `net/peer_session.{h,cc}`（QObject） | `signaling_port` | 信号槽 → std::function 回调；`localDescriptionBlocking` 逻辑保留 |
| `net/control_channel.{h,cc}`（QObject+QJson） | `control_port` | QJsonObject → 二进制协议（§5） |
| `net/h264_stream_sender.{h,cc}` | `media_port` | 本就无 Qt，改名归位；`consumeOpenTransition` 语义保留 |
| `core/openh264_codec.{h,cc}` | `video_codec_port` | 原样迁移 |
| `core/i420_frame.{h,cc}` | `VideoFrame` + 内部转换 | 容器改 `shared_ptr` 所有权；BGRA↔I420 用同一套 libyuv 调用 |
| `core/input_event.{h,cc}` | `InputEvent` + `control_protocol` | QJson 序列化 → 定长二进制序列化 |
| `session/video_quality.{h,cc}` | `quality_ladder` | 原样迁移（无 Qt） |
| `session/remote_desktop_session.cc` 中的编码线程循环 | `engine.cc` + `frame_queue` | 采集→编码流水线、EMA 统计、自动降档搬进引擎 |
| `core/input_injector` / `session/screen_capture_thread` / `ui/*` | **留在宿主层** | 不动 |

---

## 4. 线程模型

```
宿主采集线程            引擎编码线程                 libdatachannel 线程
    │ PushVideoFrameBgra     │                            │ onMessage(track)
    ▼                        ▼                            ▼
[有界队列 ~4 帧]──▶ BGRA→I420(libyuv)──▶ OpenH264──▶ RTP 打包──▶ DTLS-SRTP
  满则丢最旧                                        ▲
                                       PLI/NACK（RTCP 反向）

libdatachannel 线程                    引擎控制线程            引擎事件线程
    │ onMessage(datachannel)               │                    │
    ▼                                      ▼                    ▼
二进制协议解析 ──▶ [任务队列] ──▶ OnInput 派发      状态机汇聚 ──▶ OnState/OnError
```

- **编码线程**：唯一消费者；P 帧严格串行保压缩率；`encodeEmaMs` 在此维护，
  超预算触发自动降档（沿用现有策略：降档即时、升档需链路干净 12s、切换 3s 宽限）。
- **解码路径**：track 回调（rtc 线程）只做"RTP 解包成 access unit 后入任务队列"，
  OpenH264 解码 + `OnVideoFrame` 派发在引擎解码任务线程串行执行——避免在 rtc
  回调线程里做重活，也保证帧序。
- **事件线程**：libdatachannel 的 PC/DC 状态回调先转成轻量事件入队，引擎事件线程
  汇成 `State` 后再回调宿主。杜绝"宿主在 OnState 里直接 Close 导致 rtc 线程内析构"。
- **生命周期守卫**：沿用现有 `shared_ptr<atomic_bool> alive_` 方案——每个进入
  rtc 回调的 lambda 都持有副本，析构先置 false，迟到的回调安全退出。
- **关闭顺序**：`Close()`：置 alive=false → 关 DataChannel/Track → 关 PC →
  停编码线程（join）→ 清队列 → 清回调。幂等。

---

## 5. 控制协议 v2：JSON → 定长二进制

现状 `ControlChannel` 用 `QJsonObject` 文本消息（type: `input` / `frame_meta` /
`video_stats` / `quality_mode`），是 Qt 依赖的最后一块硬骨头，也是 DataChannel 上
无谓的 CPU 开销。新协议（binary message）：

```
偏移  大小  字段
0     1    magic  = 0x52 ('R')
1     1    version= 1
2     1    type
3     1    reserved
4     4    payload_len (u32 LE)
8     N    payload
```

| type | 名称 | payload（LE） | 方向 |
|---|---|---|---|
| 1 | InputEvent | 32B 定长结构（type/nx/ny/button/pressed/wheel/keyCode/extended） | Viewer→Host |
| 2 | FrameMeta | `{u16 w, u16 h}` | Host→Viewer |
| 3 | VideoStats | `{u32 received, u32 decoded, u32 failed}` | Viewer→Host |
| 4 | QualityMode | `{u8 mode}` | Viewer→Host |
| 5 | KeyframeReq | 空 | Viewer→Host（手动请求 IDR，补充 RTCP PLI） |

- `serialize/deserialize` 为 `control_protocol.h` 中的纯函数，带 round-trip 单测。
- 收到未知 type / bad magic：丢弃 + 计数（进 Stats），不中断通道。
- version 字节保证以后加字段可向前兼容。

---

## 6. 宿主适配层（Qt GUI 如何继续工作）

`RemoteDesktopSession` 退化为 ~150 行的薄适配器：

```cpp
// session/remote_desktop_session.cc（改造后示意）
void RemoteDesktopSession::startHost(int64_t screenId) {
  engine_ = rdc::Engine::Create(rdc::Role::Host);
  engine_->OnInput([this](const rdc::InputEvent& ev) {
    injector_.Inject(ToRd(ev));                     // Win32 注入留在宿主
    emit inputInjected();
  });
  engine_->OnState([this](rdc::State s, const std::string& d) {
    QMetaObject::invokeMethod(this, [this, s, d] {   // 回到 Qt 主线程
      emit statusText(...); if (s == rdc::State::Connected) emit connected(); ...
    }, Qt::QueuedConnection);
  });
  ...
}
bool RemoteDesktopSession::hostAcceptOfferChecked(const QString& offer, int64_t id) {
  return !!engine_->AcceptOfferCreateAnswer(offer.toStdString());
}
```

- 采集线程改为调 `engine_->PushVideoFrameBgra(...)`；
- Viewer 渲染：`OnVideoFrame` 里 I420→QImage（现有 `I420ToBgra` 转换留在宿主），
  `QMetaObject::invokeMethod` 投递到 UI 线程；
- `DiagStats` 轮询改为 `engine_->GetStats()`；
- 画质下拉框改调 `engine_->SetQualityMode()`。

CMake 变化：新增 `engine/CMakeLists.txt` 产出 `rdc` 静态库（依赖
`datachannel-static`、`openh264`、libyuv 头、`screen_capture` 仅头文件）；GUI 与
四个测试目标改为链接 `rdc`——`rd_self_test` / `rd_pair_test` 顺势变成
"无 Qt 宿主"的引擎验证样例。

---

## 7. 分期实施

| 阶段 | 内容 | 验收 |
|---|---|---|
| **P1 引擎落地** | 建 `engine/` 目录与 `rdc` 库；§3 表中组件去 Qt 迁移；门面/回调/线程模型就位；协议**暂仍 JSON**（`control_port` 内临时保留 JSON 编解码）；`RemoteDesktopSession` 改为薄适配器 | 现有 GUI 行为不变；`rd_self_test`、`rd_pair_test` 通过 |
| **P2 协议二进制化** | `control_protocol.h` + 单测；ControlChannel 切二进制；去 QJson | `rd_pair_test` 双端新旧协议互操作（版本字节兼容验证） |
| **P3 独立 CLI demo** | `examples/rdc_cli_host.cc` / `rdc_cli_viewer.cc`：文件交换 SDP、采集/渲染用最简实现，全程无 Qt | 两进程点对点出图、可注入输入 |
| **P4（可选）C ABI** | `rdc_c.h`：`rdc_engine_*` C 风格封装 + void* user_data 回调 | 供以后绑定其他语言 |

## 8. 风险与对策

| 风险 | 对策 |
|---|---|
| rtc 回调线程里触发析构（历史踩坑点） | alive_ 守卫 + 事件线程汇聚状态回调（§4） |
| 宿主在 OnVideoFrame 里做重活拖慢解码 | 契约写明"轻活"；帧对象 shared_ptr 化，拷贝决策权交宿主 |
| 编码慢导致延迟累积 | 有界队列丢旧保新（沿用），Stats.dropped 暴露给宿主 |
| 档位切换瞬间解码失败被误判丢包 | 沿用 3s 宽限期 + 强制 IDR 语义（quality_ladder 原逻辑） |
| MinGW GCC 7.3（C++17，无 C++20） | 引擎只用 C++17；`std::optional/variant` 可用，禁 `std::span` 等 |
| 二进制协议两端版本不一致 | version 字节 + 未知 type 丢弃计数；`rd_pair_test` 加互操作用例 |
| 同机双实例测试的递归镜像 | 与引擎无关，沿用 README 既有测试规程（选不同显示器） |
