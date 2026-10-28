// rd_host_service — 服务进程内常驻的被控会话（原 helper 内的引擎职责上收）。
//
// 架构（RustDesk 式）：WebRTC 引擎 + 编码 + 信令收发都活在 session 0 的服务
// 进程里，随会话切换永不消亡；控制台会话里的 helper 只负责采集（帧写共享
// 内存）与输入注入。解锁/切用户时 Windows 拆掉旧会话连带 helper，引擎与
// 主控端的 WebRTC 连接不断——新 helper 拉起后重新接上采集，主控端只看到
// 一两秒画面冻结，不再断开重连。
//
// 职责：
//   - rdc::Engine 生命周期（Offer→Answer、状态/输入/统计事件）
//   - 帧泵：共享内存 → engine->PushVideoFrameBgra
//   - 面板命名管道服务端（kPipeName，协议与旧 helper 完全一致，GUI 零改动）
//   - helper 控制管道客户端（kHelperPipeName）：采集启停/输入下发/锁屏上报/
//     屏幕信息缓存；helper 消亡重连后若会话活着自动恢复采集
#ifndef REMOTE_DESKTOP_SERVICE_HOST_SESSION_H_
#define REMOTE_DESKTOP_SERVICE_HOST_SESSION_H_

#include <functional>
#include <string>

#include "host_config.h"

namespace rdhost {

// 服务启动时调用一次：启动面板管道服务端、helper 控制客户端、帧泵与统计
// 线程，并记住 STUN 配置（引擎在 StartHost 时创建）。
void StartHostSession(const rdcfg::HostConfig& cfg);

// 服务 STOP：停会话、置停止位；无法立刻 join 的分离线程随进程退出回收。
void StopHostSession();

// 创建引擎并交换信令。返回 false = 拒绝（hostActive / offer 空），调用方
// 自行回报；Answer（或失败 error）经事件 sink 异步广播（AcceptOffer 最长
// 阻塞 10s，内部在工作线程执行，不卡调用方）。
bool StartHost(long long screenId, const std::string& offer);

// 拆掉当前会话（幂等；无会话时无操作）。收尾广播 state closed。
void StopHost(const char* reason);

void SetQualityMode(int mode);
void SetDefaultScreen(long long screenId);

// queryScreens 的应答数据源：helper 最近一次上报的 info JSON；helper 不在
// 时返回"helper offline"占位列表（主控端仍可发 Offer）。
std::string CachedHelperInfo();

// 订阅事件（answer/state/error/stats/secure JSON；引擎/helper 线程触发）。
// 信令中继与面板广播各注册一个，Start/Stop 之间有效。
void AddEventSink(std::function<void(const std::string&)> sink);

}  // namespace rdhost

#endif  // REMOTE_DESKTOP_SERVICE_HOST_SESSION_H_
