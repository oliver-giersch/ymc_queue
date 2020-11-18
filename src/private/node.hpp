#ifndef YMC_QUEUE_NODE_HPP
#define YMC_QUEUE_NODE_HPP

#include <array>
#include <atomic>

#include "private/detail.hpp"

namespace ymc::detail {
struct alignas(64) cell_t {
  std::atomic<void*> val{ nullptr };
  std::atomic<enq_req_t*> enq{ nullptr };
  std::atomic<deq_req_t*> deq{ nullptr };
};

struct node_t {
  alignas(64) std::atomic<node_t*> next{nullptr};
  alignas(64) int64_t id{ 0 };
  alignas(64) std::array<cell_t, NODE_SIZE> cells{};
};
}

#endif /* YMC_QUEUE_NODE_HPP */
