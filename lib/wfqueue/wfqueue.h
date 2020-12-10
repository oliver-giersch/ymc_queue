#ifndef WFQUEUE_H
#define WFQUEUE_H

#include "align.h"

#define EMPTY ((void *) 0)

#define WFQUEUE_NODE_SIZE (((size_t) 1 << (size_t) 10) - 2)

struct wfq_enq_t {
  long volatile id;
  void * volatile val;
} CACHE_ALIGNED;

struct wfq_deq_t {
  long volatile id;
  long volatile idx;
} CACHE_ALIGNED;

struct wfq_cell_t {
  void * volatile val;
  struct wfq_enq_t * volatile enq;
  struct wfq_deq_t * volatile deq;
  void * pad[5];
};

struct wfq_node_t {
  struct wfq_node_t * volatile next CACHE_ALIGNED;
  long id CACHE_ALIGNED;
  struct wfq_cell_t cells[WFQUEUE_NODE_SIZE] CACHE_ALIGNED;
};

struct DOUBLE_CACHE_ALIGNED wfq_queue_t {
  /** Index of the next position for enqueue. */
  volatile long Ei DOUBLE_CACHE_ALIGNED;
  /** Index of the next position for dequeue. */
  volatile long Di DOUBLE_CACHE_ALIGNED;
  /** Index of the head of the queue. */
  volatile long Hi DOUBLE_CACHE_ALIGNED;
  /** Pointer to the head node of the queue. */
  struct wfq_node_t * volatile Hp;
  /** Number of processors. */
  long nprocs;
};

struct wfq_handle_t {
  /** Pointer to the next handle. */
  struct wfq_handle_t * next;
  /** Hazard pointer. */
  unsigned long volatile hzd_node_id;
  /** Pointer to the node for enqueue. */
  struct wfq_node_t * volatile Ep;
  unsigned long enq_node_id;
  /** Pointer to the node for dequeue. */
  struct wfq_node_t * volatile Dp;
  unsigned long deq_node_id;
  /** Enqueue request. */
  struct wfq_enq_t Er CACHE_ALIGNED;
  /** Dequeue request. */
  struct wfq_deq_t Dr CACHE_ALIGNED;
  /** Handle of the next enqueuer to help. */
  struct wfq_handle_t * Eh CACHE_ALIGNED;
  long Ei;
  /** Handle of the next dequeuer to help. */
  struct wfq_handle_t * Dh;
  /** Pointer to a spare node to use, to speedup adding a new node. */
  struct wfq_node_t * spare CACHE_ALIGNED;
};

#endif /* WFQUEUE_H */
