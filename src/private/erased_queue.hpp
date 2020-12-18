#ifndef YMC_ERASED_QUEUE_HPP
#define YMC_ERASED_QUEUE_HPP

#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>
#include <deque>

#include "private/handle.hpp"

namespace ymc::detail {
struct cell_t;
struct node_t;

class erased_queue_t {
  static constexpr auto PATIENCE  = std::size_t{ 10 };
  static constexpr auto NO_HAZARD = std::numeric_limits<std::uintmax_t>::max();
  /** memory reclamation */
  void cleanup(handle_t& th);
  /** enqueue sub-procedures and helper */
  bool  enq_fast(void* elem, handle_t& thread_handle, std::intmax_t& id);
  void  enq_slow(void* elem, handle_t& thread_handle, std::intmax_t id);
  void* help_enq(cell_t& c,  handle_t& thread_handle, std::intmax_t node_id);
  /** dequeue sub-procedures and helper */
  void* deq_fast(handle_t& th, std::intmax_t& id);
  void* deq_slow(handle_t& th, std::intmax_t id);
  void  help_deq(handle_t& th, handle_t& ph);

  /** Index of the next position for enqueue. */
  alignas(128) std::atomic_intmax_t m_enq_idx{ 1 };
  /** Index of the next position for dequeue. */
  alignas(128) std::atomic_intmax_t m_deq_idx{ 1 };
  /** Index of the head of the queue. */
  alignas(128) std::atomic_intmax_t m_help_idx{ 0 };
  /** Pointer to the head node of the queue. */
  std::atomic<node_t*> m_head;
  /** Vector of all thread handles */
  std::deque<handle_t> m_handles;
  std::size_t m_max_threads;

public:
  /** constructor & destructor */
  explicit erased_queue_t(std::size_t max_threads = 128);
  ~erased_queue_t() noexcept;
  /** Enqueues an element at the queue's back. */
  void enqueue(void* elem, std::size_t thread_id);
  /** Dequeues an element from the queue's front. */
  void* dequeue(std::size_t thread_id);

  erased_queue_t(const erased_queue_t&)                  = delete;
  erased_queue_t(erased_queue_t&&)                       = delete;
  const erased_queue_t& operator=(const erased_queue_t&) = delete;
  const erased_queue_t& operator=(erased_queue_t&&)      = delete;
};
}

#endif /* YMC_ERASED_QUEUE_HPP */
