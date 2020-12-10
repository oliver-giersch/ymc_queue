#ifndef YMC_QUEUE_ORIG_HPP
#define YMC_QUEUE_ORIG_HPP

#include <vector>

#include "wfqueue.h"

extern "C" {
  struct wfq_node_t* new_node();
  void enqueue(wfq_queue_t* q, wfq_handle_t* th, void* v);
  void* dequeue(wfq_queue_t* q, wfq_handle_t* th);
};

namespace ymc_original {
template <typename T>
/** A templated wrapper for the original YMC queue C implementation by Yang. */
class queue {
public:
  using pointer = T*;

  explicit queue(size_t max_threads = 128):
    m_queue{ 1, 1, 0, nullptr, static_cast<long>(max_threads) }
  {
    auto node = ::new_node();
    auto id = static_cast<long unsigned>(node->id);
    this->m_queue.Hp = node;

    for (auto i = 0; i < max_threads; ++i) {
      this->m_handles.push_back(wfq_handle_t {
        nullptr,            // next
        (long unsigned) -1, // hzd_node_id
        node,               // Ep
        id,                 // enq_node_id
        node,               // Dp
        id,                 // deq_node_id
        { 0, nullptr },     // Er
        { 0, -1 },          // Dr
        nullptr,            // Eh
        0,                  // Ei
        nullptr,            // Dh
        new_node(),         // spare
      });
    }

    for (auto i = 0; auto& handle : this->m_handles) {
      auto next = i == max_threads - 1 ? &this->m_handles[0] : &this->m_handles[i + 1];
      handle.next = next;
      handle.enq_help_handle = next;
      handle.deq_help_handle = next;

      i += 1;
    }
  }

  ~queue() noexcept {
    // delete all remaining nodes in the queue
    auto curr = this->m_queue.Hp;
    while (curr != nullptr) {
      auto tmp = curr;
      curr = curr->next;
      delete tmp;
    }

    // delete any remaining thread-local spare nodes
    for (auto& handle : this->m_handles) {
      delete handle.spare;
    }
  }

  queue(const queue&)                  = delete;
  queue(queue&&)                       = delete;
  const queue& operator=(const queue&) = delete;
  const queue& operator=(queue&&)      = delete;

  void enqueue(pointer elem, size_t thread_id) {
    auto th = &this->m_handles[thread_id];
    ::enqueue(&this->m_queue, th, reinterpret_cast<void*>(elem));
  }

  pointer dequeue(size_t thread_id) {
    auto th = &this->m_handles[thread_id];
    return reinterpret_cast<pointer>(::dequeue(&this->m_queue, th));
  }

private:
  wfq_queue_t m_queue;
  std::vector<wfq_handle_t> m_handles{};
};
}

#endif /* YMC_QUEUE_ORIG_HPP */
