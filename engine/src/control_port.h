// rdc — DataChannel 控制通道封装（去 Qt 化的 ControlChannel）。角色由调用
// 方决定：Viewer createDataChannel，Host onDataChannel 接收。
#ifndef RDC_SRC_CONTROL_PORT_H_
#define RDC_SRC_CONTROL_PORT_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rtc/rtc.hpp>

#include "control_protocol.h"

namespace rdc {

class ControlPort {
 public:
  using OpenFn = std::function<void()>;
  using ClosedFn = std::function<void()>;
  using ErrorFn = std::function<void(const std::string&)>;
  using MessageFn = std::function<void(const ctrl::Message&)>;

  ControlPort();
  ~ControlPort();

  // 回调在 libdatachannel 线程触发；设置要在 CreateOn/AcceptOn 之前完成。
  void SetCallbacks(OpenFn onOpen, ClosedFn onClosed, ErrorFn onError,
                    MessageFn onMessage);

  // Viewer：建通道（随 offer 协商）。
  void CreateOn(rtc::PeerConnection& pc);
  // Host：接收远端建的通道。
  void AcceptOn(rtc::PeerConnection& pc);

  // 发一条二进制消息；通道未开时静默丢弃并返回 false。
  bool Send(const std::vector<uint8_t>& bytes);

  bool IsOpen() const;

  void Close();

  uint32_t badMessages() const {
    return badMessages_.load(std::memory_order_relaxed);
  }

 private:
  void Wire(rtc::DataChannel& dc);

  // dc_ 在 libdatachannel 线程（onDataChannel）赋值，而 Send()/IsOpen() 可能
  // 在任意线程调用，互斥保护。
  mutable std::mutex mtx_;
  std::shared_ptr<rtc::DataChannel> dc_;

  std::mutex cbMtx_;
  OpenFn onOpen_;
  ClosedFn onClosed_;
  ErrorFn onError_;
  MessageFn onMessage_;

  std::atomic<uint32_t> badMessages_{0};
  // 每个回调都持有；析构置 false，让迟到回调安全退出而不摸死对象。
  std::shared_ptr<std::atomic_bool> alive_;
};

}  // namespace rdc
#endif  // RDC_SRC_CONTROL_PORT_H_
