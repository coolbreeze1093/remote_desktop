// rd_host_service — 直连信令中继（服务内，session 0）。
//
// 把 GUI 面板承担的 TCP 信令（RDC1 帧：Offer/Answer/BYE，协议见
// src/session/direct_signaling.h）搬进服务本体：
//   主控端 ──TCP:48010──▶ 服务（本文件 + host_session 内的引擎）
//
// 为什么放服务里：面板与 helper 都活在交互会话里，锁屏解锁/切换用户/RDP
// 接管时随会话消亡；服务（session 0）永远在线，信令监听与 WebRTC 引擎
// 都不随会话切换消亡。引擎常驻后 Offer 立即应答——旧架构"押住 Offer 等
// helper 管道"的环节已不存在；helper 不在线时画面等其拉起后续上（host_
// session 对控制管道重连即恢复采集），主控端连接始终保活。
//
// 语义与旧面板信令一致：同一时刻只服务一个主控端，占线时新连接立即关闭；
// Answer 回完 TCP 保活，等 BYE 或断连即 StopHost；引擎报 closed/failed/
// error 时主动断开信令连接让主控端快速失败（配合其自动重连）。
#ifndef REMOTE_DESKTOP_SERVICE_SIGNAL_RELAY_H_
#define REMOTE_DESKTOP_SERVICE_SIGNAL_RELAY_H_

namespace rdrelay {

// 启动信令监听（一条常驻 accept 线程）并注册引擎事件收尾 sink；
// 须在 rdhost::StartHostSession 之后调用。重复调用无副作用。
void StartSignalRelay(unsigned short port);
// 停止并回收线程（服务 STOP 时调用；无法立刻 join 的线程随进程退出回收）。
void StopSignalRelay();

}  // namespace rdrelay

#endif  // REMOTE_DESKTOP_SERVICE_SIGNAL_RELAY_H_
