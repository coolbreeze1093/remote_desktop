// rdc — 有界队列：满时丢最旧、保最新（编码/解码队列共用）。push 永不阻塞，
// 延迟不随消费速度累积；Stop 后 PopWait 返回 false 用于退出消费线程。
#ifndef RDC_SRC_FRAME_QUEUE_H_
#define RDC_SRC_FRAME_QUEUE_H_

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace rdc {

template <typename T>
class DropOldestQueue {
 public:
  explicit DropOldestQueue(size_t capacity) : capacity_(capacity) {}

  // 返回本次因满而淘汰的元素个数（调用方累计进 dropped 统计）。
  size_t Push(T&& item) {
    size_t dropped = 0;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      while (queue_.size() >= capacity_ && !queue_.empty()) {
        queue_.pop_front();
        ++dropped;
      }
      queue_.push_back(std::move(item));
    }
    cv_.notify_one();
    return dropped;
  }

  // 阻塞取队首；Stop() 且队列空后返回 false（消费线程据此退出）。
  bool PopWait(T& out) {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
    if (queue_.empty()) {
      return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      stopped_ = true;
    }
    cv_.notify_all();
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(mtx_);
    queue_.clear();
  }

 private:
  size_t capacity_;
  bool stopped_ = false;
  std::deque<T> queue_;
  std::mutex mtx_;
  std::condition_variable cv_;
};

}  // namespace rdc
#endif  // RDC_SRC_FRAME_QUEUE_H_
