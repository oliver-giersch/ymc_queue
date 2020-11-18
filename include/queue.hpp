#ifndef YMC_QUEUE_QUEUE_HPP
#define YMC_QUEUE_QUEUE_HPP

#include "private/queue_private.hpp"

namespace ymc {
template <typename T>
class queue {
public:
  using pointer = T*;

  explicit queue(size_t max_threads = 128): m_queue(max_threads) {}
  ~queue() noexcept = default;

  queue(const queue&)                  = delete;
  queue(queue&&)                       = delete;
  const queue& operator=(const queue&) = delete;
  const queue& operator=(queue&&)      = delete;

  void enqueue(pointer elem, size_t thread_id) {
    this->m_queue.enqueue(reinterpret_cast<void*>(elem), thread_id);
  }

  pointer dequeue(size_t thread_id) {
    return reinterpret_cast<pointer>(this->m_queue.dequeue(thread_id));
  }

private:
  detail::erased_queue_t m_queue;
};
}

#endif /* YMC_QUEUE_QUEUE_HPP */
