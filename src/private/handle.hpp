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
    thread_id{ thread_id }, Ep{ node }, Dp{ node }, handles{ max_threads }
  {
    for (auto i = 0; i < max_threads; ++i) {
      this->handles.push_back(nullptr);
    }
  }

  /** Thread handle id. */
  std::size_t thread_id;
  /** Pointer to the next handle. */
  handle_t* next{ nullptr };
  /** Hazard pointer. */
  std::atomic_uint64_t hzd_node_id{ MAX_U64 };
  /** Pointer to the node for enqueue. */
  std::atomic<node_t*> Ep;
  uint64_t enq_node_id{ 0 };
  /** Pointer to the node for dequeue. */
  std::atomic<node_t*> Dp;
  uint64_t deq_node_id{ 0 };
  /** Enqueue request. */
  enq_req_t Er{ 0, nullptr };
  /** Dequeue request. */
  deq_req_t Dr{ 0, -1 };
  /** Handle of the next enqueue to help. */
  handle_t* Eh{ nullptr };
  int64_t Ei{ 0 };
  /** Handle of the next dequeue to help. */
  handle_t* Dh{ nullptr };
  /** Pointer to a spare node to use, to speedup adding a new node. */
  node_t* spare{ new node_t() };
  /** */
  std::vector<handle_t*> handles;
};
}

#endif /* YMC_QUEUE_HANDLE_HPP */
