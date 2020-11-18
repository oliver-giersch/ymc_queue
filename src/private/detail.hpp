#ifndef YMC_QUEUE_DETAIL_HPP
#define YMC_QUEUE_DETAIL_HPP

#include <atomic>

namespace ymc::detail {
/** The size of each node's cell array. */
constexpr std::size_t NODE_SIZE = 1024;
/** A enqueue request. */
struct alignas(64) enq_req_t {
  std::atomic_int64_t id;
  std::atomic<void*> val;
};
/** A dequeue request. */
struct alignas(64) deq_req_t {
  std::atomic_int64_t id;
  std::atomic_int64_t idx;
};
}

#endif /* YMC_QUEUE_DETAIL_HPP */
