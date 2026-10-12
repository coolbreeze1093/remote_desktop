// rdc — DataChannel 控制通道：收发二进制控制协议消息（v2），回调直接在
// libdatachannel 线程触发，由 Engine 转派发线程后回调宿主。
#include "control_port.h"

#include "control_protocol.h"
#include "rdc/log.h"

namespace rdc {

ControlPort::ControlPort()
    : alive_(std::make_shared<std::atomic_bool>(true)) {}

ControlPort::~ControlPort() {
  alive_->store(false, std::memory_order_release);
  std::lock_guard<std::mutex> lock(mtx_);
  dc_.reset();
}

void ControlPort::CreateOn(rtc::PeerConnection& pc) {
  auto dc = pc.createDataChannel("control");
  {
    std::lock_guard<std::mutex> lock(mtx_);
    dc_ = dc;
  }
  Wire(*dc);
}

void ControlPort::AcceptOn(rtc::PeerConnection& pc) {
  const auto alive = alive_;
  pc.onDataChannel([this, alive](std::shared_ptr<rtc::DataChannel> dc) {
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mtx_);
      dc_ = dc;
    }
    Wire(*dc);
  });
}

void ControlPort::SetCallbacks(OpenFn onOpen, ClosedFn onClosed,
                               ErrorFn onError, MessageFn onMessage) {
  std::lock_guard<std::mutex> lock(cbMtx_);
  onOpen_ = std::move(onOpen);
  onClosed_ = std::move(onClosed);
  onError_ = std::move(onError);
  onMessage_ = std::move(onMessage);
}

void ControlPort::Wire(rtc::DataChannel& dc) {
  auto* self = this;
  const auto alive = alive_;
  dc.onOpen([self, alive]() {
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    rdcDebug("ctrl", "dc open");
    OpenFn cb;
    {
      std::lock_guard<std::mutex> lock(self->cbMtx_);
      cb = self->onOpen_;
    }
    if (cb) {
      cb();
    }
  });
  dc.onClosed([self, alive]() {
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    rdcDebug("ctrl", "dc closed");
    ClosedFn cb;
    {
      std::lock_guard<std::mutex> lock(self->cbMtx_);
      cb = self->onClosed_;
    }
    if (cb) {
      cb();
    }
  });
  dc.onError([self, alive](std::string err) {
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    rdcDebug("ctrl", "dc error: %s", err.c_str());
    ErrorFn cb;
    {
      std::lock_guard<std::mutex> lock(self->cbMtx_);
      cb = self->onError_;
    }
    if (cb) {
      cb(std::move(err));
    }
  });
  dc.onMessage([self, alive](rtc::message_variant msg) {
    if (!alive->load(std::memory_order_acquire)) {
      return;
    }
    if (!std::holds_alternative<rtc::binary>(msg)) {
      // v2 协议只收二进制；文本一律忽略（旧 JSON 对端不再支持）。
      rdcDebug("ctrl", "ignored text message (protocol v2 is binary)");
      return;
    }
    const rtc::binary& raw = std::get<rtc::binary>(msg);
    ctrl::Message m;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(raw.data());
    if (!ctrl::Decode(bytes, raw.size(), m)) {
      // 坏消息：丢弃 + 限频日志。
      const uint32_t total =
          self->badMessages_.fetch_add(1, std::memory_order_relaxed) + 1;
      if (total <= 3 || total % 100 == 0) {
        rdcDebug("ctrl", "bad control message #%u (size=%zu)", total,
                 raw.size());
      }
      return;
    }
    MessageFn cb;
    {
      std::lock_guard<std::mutex> lock(self->cbMtx_);
      cb = self->onMessage_;
    }
    if (cb) {
      cb(m);
    }
  });
}

bool ControlPort::Send(const std::vector<uint8_t>& bytes) {
  std::shared_ptr<rtc::DataChannel> dc;
  {
    std::lock_guard<std::mutex> lock(mtx_);
    dc = dc_;
  }
  if (!dc || !dc->isOpen() || bytes.empty()) {
    return false;
  }
  try {
    rtc::binary payload(reinterpret_cast<const std::byte*>(bytes.data()),
                        reinterpret_cast<const std::byte*>(bytes.data() +
                                                           bytes.size()));
    return dc->send(std::move(payload));
  } catch (const std::exception&) {
    // isOpen() 与 send() 之间通道可能关闭；丢消息即可。
    return false;
  }
}

bool ControlPort::IsOpen() const {
  std::lock_guard<std::mutex> lock(mtx_);
  return dc_ && dc_->isOpen();
}

void ControlPort::Close() {
  std::shared_ptr<rtc::DataChannel> dc;
  {
    std::lock_guard<std::mutex> lock(mtx_);
    dc = dc_;
    dc_.reset();
  }
  if (dc) {
    try {
      dc->close();
    } catch (const std::exception&) {
    }
  }
}

}  // namespace rdc
