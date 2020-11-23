#ifndef YMC_QUEUE_ORDERING_HPP
#define YMC_QUEUE_ORDERING_HPP

#define SEQ_CST_CAS std::memory_order_seq_cst, std::memory_order_seq_cst

#define RLX_CAS std::memory_order_relaxed, std::memory_order_relaxed
#define ACQ_CAS std::memory_order_acquire, std::memory_order_relaxed
#define REL_ACQ_CAS std::memory_order_release, std::memory_order_acquire

#define SEQ_CST std::memory_order_seq_cst

#define RLX std::memory_order_relaxed
#define ACQ std::memory_order_acquire
#define REL std::memory_order_release

#endif /* YMC_QUEUE_ORDERING_HPP */
