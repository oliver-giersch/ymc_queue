#ifndef YMC_QUEUE_HANDLE_HPP
#define YMC_QUEUE_HANDLE_HPP

#include <atomic>
#include <limits>
#include <vector>

#include "private/detail.hpp"
#include "private/node.hpp"

namespace ymc::detail {
constexpr auto MAX_U64 = std::numeric_limits<uint64_t>::max();
struct handle_t {
  /** constructor */
  handle_t(size_t thread_id, node_t* node, size_t max_threads):
      thread_id{ thread_id }, tail{ node }, head{ node }, peer_handles{ max_threads }
  {
    for (auto i = 0; i < max_threads; ++i) {
      this->peer_handles.push_back(nullptr);
    }
  }

  /** Thread handle id. */
  std::size_t thread_id;
  /** Pointer to the next handle. */
  handle_t* next{ nullptr };
  /** Hazard pointer. */
  std::atomic_uint64_t hzd_node_id{ MAX_U64 };
  /** Pointer to the node for enqueue. */
  std::atomic<node_t*> tail;
  uint64_t tail_node_id{ 0 };
  /** Pointer to the node for dequeue. */
  std::atomic<node_t*> head;
  uint64_t head_node_id{ 0 };
  /** Enqueue request. */
  alignas(64) enq_req_t enq_req{ 0, nullptr };
  /** Dequeue request. */
  alignas(64) deq_req_t deq_req{ 0, -1 };
  /** Handle of the next enqueue to help. */
  alignas(64) handle_t* enq_help_handle{ nullptr };
  int64_t Ei{ 0 };
  /** Handle of the next dequeue to help. */
  handle_t* deq_help_handle{ nullptr };
  /** Pointer to a spare node to use, to speedup adding a new node. */
  node_t* spare_node{ new node_t() };
  /** Storage for temporary thread handles during cleanup. */
  std::vector<handle_t*> peer_handles;
};
}

#endif /* YMC_QUEUE_HANDLE_HPP */
