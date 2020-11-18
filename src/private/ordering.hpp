#ifndef YMC_QUEUE_ORDERING_HPP
#define YMC_QUEUE_ORDERING_HPP

#define RLX_CAS std::memory_order_relaxed, std::memory_order_relaxed
#define ACQ_CAS std::memory_order_acquire, std::memory_order_relaxed
#define REL_ACQ_CAS std::memory_order_release, std::memory_order_acquire

#define RLX std::memory_order_relaxed
#define ACQ std::memory_order_acquire
#define REL std::memory_order_release

#endif /* YMC_QUEUE_ORDERING_HPP */
