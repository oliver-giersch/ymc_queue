#ifndef YMC_QUEUE_HPP
#define YMC_QUEUE_HPP

#include "private/erased_queue.hpp"

namespace ymc {
template <typename T>
class queue {
  /** the internal queue representation */
  detail::erased_queue_t m_queue;
public:
  using pointer = T*;
  /** constructor & destructor */
  explicit queue(std::size_t max_threads = 128) : m_queue{ max_threads } {}
  ~queue() noexcept = default;

  /** Enqueues the given `elem` the queue's back. */
  void enqueue(pointer elem, std::size_t thread_id) {
    this->m_queue.enqueue(reinterpret_cast<void*>(elem), thread_id);
  }

  /** Dequeues an element from the queue's front. */
  pointer dequeue(size_t thread_id) {
    return reinterpret_cast<pointer>(this->m_queue.dequeue(thread_id));
  }

  /** deleted copy/move constructors & assignment operators */
  queue(const queue&)                  = delete;
  queue(queue&&)                       = delete;
  const queue& operator=(const queue&) = delete;
  const queue& operator=(queue&&)      = delete;
};
}

#endif /* YMC_QUEUE_HPP */
