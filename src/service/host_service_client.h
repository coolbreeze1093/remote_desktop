// remote_desktop — 被控服务的控制面板客户端（Qt 侧）。
//
// 连接 rd_host_service helper 的命名管道：后台线程负责连接/重连/收发，
// 事件以 Qt 信号投递到 UI 线程；指令由 UI 线程直接写入（未连接则丢弃）。
// 协议见 service/pipe_proto.h（4 字节小端长度 + UTF-8 JSON）。
#ifndef REMOTE_DESKTOP_SRC_SERVICE_HOST_SERVICE_CLIENT_H_
#define REMOTE_DESKTOP_SRC_SERVICE_HOST_SERVICE_CLIENT_H_

#include <QObject>
#include <QString>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace rd {

class HostServiceClient : public QObject {
  Q_OBJECT
 public:
  explicit HostServiceClient(QObject* parent = nullptr);
  ~HostServiceClient() override;

  void start();
  // 发送一条指令（JSON 文本）。未连接时静默丢弃（面板按钮只在连接后才可用）。
  void send(const std::string& json);

 signals:
  // 连接建立后 helper 首发的 info（版本/提权/屏幕列表），以及重连后的更新。
  void serviceInfo(const QString& json);
  // 其余事件：answer/state/secure/error/stats。
  void eventReceived(const QString& json);
  // 连接断开（helper 未运行或服务重启中；内部会自动重连）。
  // 首次确认连不上管道（服务未装/helper 未起）时也发一次，让面板从
  // "检测中…"落到"未运行"的确定状态。
  void serviceGone();
  // SCM 层状态（与管道通断独立）：服务是否已注册、是否运行中，变化时发。
  // 管道连不上时面板据此区分"未安装"和"装了但 helper 没起来"。
  void serviceStatus(bool installed, bool running);

 private:
  void run();
  // 查询 SCM 里 RdHostService 的注册/运行状态（只读，普通用户可用）。
  static void ProbeScm(bool* installed, bool* running);

  std::atomic_bool running_{false};
  std::thread worker_;

  std::mutex mu_;                 // 保护 pipe_ 指针
  void* pipe_ = nullptr;          // HANDLE（避免拖 windows.h 进头文件）
  bool absentReported_ = false;   // 仅 worker 线程触碰：避免每 2s 重复回报
  bool scmProbed_ = false;        // 以下三项仅 worker 线程：SCM 探测去重
  bool scmInstalled_ = false;
  bool scmRunning_ = false;
};

}  // namespace rd

#endif  // REMOTE_DESKTOP_SRC_SERVICE_HOST_SERVICE_CLIENT_H_
