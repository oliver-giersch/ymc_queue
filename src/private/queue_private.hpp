#ifndef YMC_QUEUE_QUEUE_PRIVATE_HPP
#define YMC_QUEUE_QUEUE_PRIVATE_HPP

#include <atomic>
#include <vector>
#include <deque>

#include "private/handle.hpp"

namespace ymc::detail {
struct cell_t;
struct node_t;

class erased_queue_t {
public:
  explicit erased_queue_t(std::size_t max_threads = 128);
  ~erased_queue_t() noexcept;

  void enqueue(void* elem, std::size_t thread_id);
  void* dequeue(std::size_t thread_id);

  erased_queue_t(const erased_queue_t&)                  = delete;
  erased_queue_t(erased_queue_t&&)                       = delete;
  const erased_queue_t& operator=(const erased_queue_t&) = delete;
  const erased_queue_t& operator=(erased_queue_t&&)      = delete;

private:
  static constexpr std::size_t PATIENCE = 10;

  void cleanup(handle_t& th);

  bool  enq_fast(void* elem, handle_t& th, int64_t& id);
  void  enq_slow(void* elem, handle_t& th, int64_t id);
  void* help_enq(cell_t& c, handle_t& th, int64_t i);

  void* deq_fast(handle_t& th, int64_t& id);
  void* deq_slow(handle_t& th, int64_t id);
  void  help_deq(handle_t& th, handle_t& ph);

  /** Index of the next position for enqueue. */
  alignas(128) std::atomic_int64_t m_enq_idx{ 1 };
  /** Index of the next position for dequeue. */
  alignas(128) std::atomic_int64_t m_deq_idx{ 1 };
  /** Index of the head of the queue. */
  alignas(128) std::atomic_int64_t m_help_idx{ 0 };
  /** Pointer to the head node of the queue. */
  std::atomic<node_t*> m_head;
  /** Vector of all thread handles */
  std::deque<handle_t> m_handles;
  std::size_t m_max_threads;
};
}

#endif /* YMC_QUEUE_QUEUE_PRIVATE_HPP */
