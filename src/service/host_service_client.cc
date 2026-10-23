#include "service/host_service_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "pipe_proto.h"

namespace rd {

namespace {
constexpr int kReconnectMs = 2000;
constexpr int kPumpMs = 120;
}  // namespace

HostServiceClient::HostServiceClient(QObject* parent) : QObject(parent) {}

HostServiceClient::~HostServiceClient() {
  running_ = false;
  if (worker_.joinable()) {
    worker_.join();
  }
}

void HostServiceClient::start() {
  if (running_.exchange(true)) {
    return;
  }
  worker_ = std::thread([this] { run(); });
}

void HostServiceClient::send(const std::string& json) {
  // 读（worker 线程）与写（UI 线程）方向独立，字节型管道允许并发读写；
  // mu_ 只保护 pipe_ 句柄指针的更换。
  std::lock_guard<std::mutex> lk(mu_);
  HANDLE h = static_cast<HANDLE>(pipe_);
  if (h) {
    rdpipe::WriteFrame(h, json);
  }
}

// 查询 SCM：服务是否已注册、是否运行中。只读权限，普通用户可用；
// 打不开 SCM（权限异常）时按"未安装"处理，面板仍可退回管道探测结果。
void HostServiceClient::ProbeScm(bool* installed, bool* running) {
  *installed = false;
  *running = false;
#ifndef SC_MANAGER_CONNECT
#define SC_MANAGER_CONNECT 0x0001  // MinGW 头缺失时补
#endif
  SC_HANDLE sc = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!sc) {
    return;
  }
  SC_HANDLE svc = OpenServiceW(sc, rdpipe::kServiceName, SERVICE_QUERY_STATUS);
  if (svc) {
    *installed = true;
    SERVICE_STATUS st = {};
    if (QueryServiceStatus(svc, &st) &&
        st.dwCurrentState == SERVICE_RUNNING) {
      *running = true;
    }
    CloseServiceHandle(svc);
  }
  CloseServiceHandle(sc);
}

void HostServiceClient::run() {
  std::string rx;
  for (;;) {
    if (!running_) {
      return;
    }
    HANDLE h = CreateFileW(rdpipe::kPipeName, GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      // 服务未装 / helper 未起 / 上一连接未释放，稍后重试。首次失败要回报
      // 一次"未运行"，否则面板永远停在"检测中…"（装没装都看不出来）。
      // 顺带探测 SCM 注册状态（2s 一次足够），让面板区分"未安装"与
      // "装了但 helper 没起来"。
      bool installed = false;
      bool running = false;
      ProbeScm(&installed, &running);
      if (!scmProbed_ || installed != scmInstalled_ ||
          running != scmRunning_) {
        scmProbed_ = true;
        scmInstalled_ = installed;
        scmRunning_ = running;
        emit serviceStatus(installed, running);
      }
      if (!absentReported_) {
        absentReported_ = true;
        emit serviceGone();
      }
      Sleep(kReconnectMs);
      continue;
    }
    absentReported_ = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      pipe_ = h;
    }
    rx.clear();
    rdpipe::WriteFrame(h, "{\"type\":\"hello\"}");

    bool alive = true;
    while (alive && running_) {
      if (!rdpipe::DrainAvailable(h, &rx)) {
        alive = false;
        break;
      }
      std::string msg;
      while (rdpipe::PopFrame(&rx, &msg)) {
        const std::string type = rdpipe::JsonGetString(msg, "type");
        const QString payload = QString::fromStdString(msg);
        if (type == "info") {
          emit serviceInfo(payload);
        } else {
          emit eventReceived(payload);
        }
      }
      Sleep(kPumpMs);
    }

    {
      std::lock_guard<std::mutex> lk(mu_);
      pipe_ = nullptr;
    }
    CloseHandle(h);
    if (running_) {
      emit serviceGone();
      Sleep(kReconnectMs);
    }
  }
}

}  // namespace rd
