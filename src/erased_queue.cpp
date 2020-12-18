#include "private/erased_queue.hpp"

#include <atomic>
#include <stdexcept>

namespace ymc::detail {
constexpr auto relaxed = std::memory_order_relaxed;
constexpr auto acquire = std::memory_order_acquire;
constexpr auto release = std::memory_order_release;
constexpr auto seq_cst = std::memory_order_seq_cst;

struct find_cell_result_t {
  cell_t& cell;
  node_t& curr;
};

template<typename T>
constexpr T* top_ptr() {
  return reinterpret_cast<T*>(std::numeric_limits<std::uintmax_t>::max());
}

/** Check the given peer's current hazard node id and return the matching node pointer. */
node_t* check(
    const std::atomic_uintmax_t& peer_hzd_node_id,
    node_t* curr,
    node_t* old
) {
  // read the peer's current hazard node id
  const auto hzd_node_id = peer_hzd_node_id.load(acquire);
  // the peer's hazard id lags behind the current node
  if (hzd_node_id < curr->id) {
    auto tmp = old;
    // advance curr until the first node protected by the peer
    while (tmp->id < hzd_node_id) {
      tmp = tmp->next;
    }
    curr = tmp;
  }

  return curr;
}

/** Advances a peer thread's head/tail pointer */
node_t* update(
    std::atomic<node_t*>& peer_node,
    const std::atomic_uintmax_t& peer_hzd_node_id,
    node_t* curr,
    node_t* old
) {
  // check the peer's current node pointer
  auto node = peer_node.load(acquire);
  // if the peer is lagging behind, update the pointer
  if (node->id < curr->id) {
    if (!peer_node.compare_exchange_strong(node, curr, seq_cst, seq_cst)) {
      if (node->id < curr->id) {
        curr = node;
      }
    }

    curr = check(peer_hzd_node_id, curr, old);
  }

  return curr;
}

/** Searches for the node & cell matching the given idx value. */
find_cell_result_t find_cell(
    const std::atomic<node_t*>& ptr,
    handle_t& thread_handle,
    std::intmax_t idx
) {
  auto curr = ptr.load(relaxed);
  // search the node containing the cell for the idx value
  for (auto j = curr->id; j < idx / NODE_SIZE; ++j) {
    auto next = curr->next.load(relaxed);
    // if no node for the searched idx exists yet, install a new one
    if (next == nullptr) {
      auto tmp = thread_handle.spare_node;
      // use the current spare node if there is one
      if (tmp == nullptr) {
        tmp = new node_t();
        thread_handle.spare_node = tmp;
      }
      // set the appropriate node id
      tmp->id = j + 1;
      // attempt to install it and proceed
      if (curr->next.compare_exchange_strong(next, tmp, release, acquire)) {
        next = tmp;
        thread_handle.spare_node = nullptr;
      }
    }

    curr = next;
  }
  // return both the cell and the node pointer (reference)
  return { curr->cells[idx % NODE_SIZE], *curr };
}

/********** constructor & destructor **************************************************************/

erased_queue_t::erased_queue_t(std::size_t max_threads):
  m_handles{ }, m_max_threads{ max_threads }
{
  if (max_threads == 0) {
    throw std::invalid_argument("max_threads must be at least 1");
  }

  // install empty head node
  auto node = new node_t();
  this->m_head.store(node, relaxed);

  for (auto i = 0; i < max_threads; ++i) {
    this->m_handles.emplace_back(node, max_threads);
  }

  for (auto i = 0; auto& handle : this->m_handles) {
    auto next = i == max_threads - 1
        ? &this->m_handles[0]
        : &this->m_handles[i + 1];
    handle.next = next;
    handle.enq_help_handle = next;
    handle.deq_help_handle = next;

    i += 1;
  }
}

erased_queue_t::~erased_queue_t() noexcept {
  // delete all remaining nodes in the queue
  auto curr = this->m_head.load(relaxed);
  while (curr != nullptr) {
    auto tmp = curr;
    curr = curr->next.load(relaxed);
    delete tmp;
  }

  // delete any remaining thread-local spare nodes
  for (auto& handle : this->m_handles) {
    delete handle.spare_node;
  }
}

/********** public methods ************************************************************************/

void erased_queue_t::enqueue(void* elem, std::size_t thread_id) {
  auto& th = this->m_handles[thread_id];
  th.hzd_node_id.store(th.tail_node_id, relaxed);

  std::intmax_t id = 0;
  bool success = false;

  for (auto patience = 0; patience < PATIENCE; ++patience) {
    if ((success = this->enq_fast(elem, th, id))) {
      break;
    }
  }

  if (!success) {
    this->enq_slow(elem, th, id);
  }

  th.tail_node_id = th.tail.load(relaxed)->id;
  th.hzd_node_id.store(NO_HAZARD, release);
}

void* erased_queue_t::dequeue(std::size_t thread_id) {
  auto& th = this->m_handles[thread_id];
  th.hzd_node_id.store(th.head_node_id, relaxed);

  std::intmax_t id = 0;
  void* res = nullptr;

  for (auto patience = 0; patience < PATIENCE; ++patience) {
    if ((res = this->deq_fast(th, id)) != top_ptr<void>()) {
      break;
    }
  }

  if (res == top_ptr<void>()) {
    res = this->deq_slow(th, id);
  }

  if (res != nullptr) {
    this->help_deq(th, *th.deq_help_handle);
    th.deq_help_handle = th.deq_help_handle->next;
  }

  th.head_node_id = th.head.load(relaxed)->id;
  th.hzd_node_id.store(NO_HAZARD, release);

  if (th.spare_node == nullptr) {
    this->cleanup(th);
    th.spare_node = new node_t();
  }

  return res;
}

/********** private methods ***********************************************************************/

void erased_queue_t::cleanup(handle_t& th) {
 auto oid = this->m_help_idx.load(acquire);
 auto new_node = th.head.load(relaxed);

 if (oid == -1) {
   return;
 }

 if (new_node->id - oid < (this->m_max_threads * 2)) {
   return;
 }

 if (
     !this->m_help_idx.compare_exchange_strong(oid, -1, acquire, relaxed)
 ) {
   return;
 }

 // from here on only one thread

 auto lDi = this->m_deq_idx.load(relaxed);
 auto lEi = this->m_enq_idx.load(relaxed);

 while (
     lEi <= lDi
     && !this->m_enq_idx.compare_exchange_weak(
         lEi, lDi + 1, relaxed, relaxed)
 ) {}

 auto old_node = this->m_head.load(relaxed);
 auto ph = &th;
 auto i = 0;

 do {
   new_node = check(ph->hzd_node_id, new_node, old_node);
   new_node = update(ph->tail, ph->hzd_node_id, new_node, old_node);
   new_node = update(ph->head, ph->hzd_node_id, new_node, old_node);

   th.peer_handles[i++] = ph;
   ph = ph->next;
 } while (new_node->id > oid && ph != &th);

 while (new_node->id > oid && --i >= 0) {
   new_node = check(th.peer_handles[i]->hzd_node_id, new_node, old_node);
 }

 const auto nid = new_node->id;

  if (nid <= oid) {
    this->m_help_idx.store(oid, release);
  } else {
    this->m_head.store(new_node, relaxed);
    this->m_help_idx.store(nid, release);

    while (old_node != new_node) {
      auto tmp = old_node->next.load(relaxed);
      delete old_node;
      old_node = tmp;
    }
  }
}

/********** private methods (enqueue) *************************************************************/

bool erased_queue_t::enq_fast(void* elem, handle_t& thread_handle, std::intmax_t& id) {
  const auto i = this->m_enq_idx.fetch_add(1, seq_cst);
  auto [cell, curr] = find_cell(thread_handle.tail, thread_handle, i);
  thread_handle.tail.store(&curr, relaxed);

  void* expected = nullptr;
  if (cell.val.compare_exchange_strong(expected, elem, relaxed, relaxed)) {
    return true;
  } else {
    id = i;
    return false;
  }
}

void erased_queue_t::enq_slow(void* elem, handle_t& thread_handle, std::intmax_t id) {
  auto& enq = thread_handle.enq_req;
  enq.val.store(elem, relaxed);
  enq.id.store(id, release);

  std::intmax_t i;
  do {
    i = this->m_enq_idx.fetch_add(1, relaxed);
    auto [cell, _ignore] = find_cell(thread_handle.tail, thread_handle, i);

    enq_req_t* expected = nullptr;
    if (
        cell.enq_req.compare_exchange_strong(expected, &enq, seq_cst, seq_cst)
        && cell.val.load(relaxed) != top_ptr<enq_req_t>()
    ) {
      if (enq.id.compare_exchange_strong(id, -i, relaxed, relaxed)) {
        id = -i;
      }

      break;
    }
  } while (enq.id.load(relaxed) > 0);

  id = -enq.id.load(relaxed);
  auto [cell, curr] = find_cell(thread_handle.tail, thread_handle, id);
  thread_handle.tail.store(&curr, relaxed);

  if (id > i) {
    auto lEi = this->m_enq_idx.load(relaxed);
    while (
        lEi <= id
        && !this->m_enq_idx.compare_exchange_weak(
            lEi, id + 1, relaxed, relaxed)
    ) {}
  }

  cell.val.store(elem, relaxed);
}

void* erased_queue_t::help_enq(cell_t& cell, handle_t& thread_handle, std::intmax_t node_id) {
  auto res = cell.val.load(acquire);

  if (res != top_ptr<void>() && res != nullptr) {
    return res;
  }

  if (
      res == nullptr
      && !cell.val.compare_exchange_strong(
          res, top_ptr<void>(), seq_cst, seq_cst)
  ) {
    if (res != top_ptr<void>()) {
      return res;
    }
  }

  auto enq = cell.enq_req.load(relaxed);

  if (enq == nullptr) {
    auto ph = thread_handle.enq_help_handle;
    auto pe = &ph->enq_req;
    auto id = pe->id.load(relaxed);

    if (thread_handle.Ei != 0 && thread_handle.Ei != id) {
      thread_handle.Ei = 0;
      thread_handle.enq_help_handle = ph->next;
      ph = thread_handle.enq_help_handle;
      pe = &ph->enq_req;
      id = pe->id;
    }

    if (
        id > 0 && id <= node_id
        && !cell.enq_req.compare_exchange_strong(enq, pe, relaxed, relaxed)
        && enq != pe
    ) {
      thread_handle.Ei = id;
    } else {
      thread_handle.Ei = 0;
      thread_handle.enq_help_handle = ph->next;
    }

    if (
        enq == nullptr && cell.enq_req.compare_exchange_strong(
            enq, top_ptr<enq_req_t>(), relaxed, relaxed
        )
    ) {
      enq = top_ptr<enq_req_t>();
    }
  }

  if (enq == top_ptr<enq_req_t>()) {
    return (this->m_enq_idx.load(relaxed) <= node_id ? nullptr : top_ptr<void>());
  }

  auto enq_id = enq->id.load(acquire);
  const auto enq_val = enq->val.load(acquire);

  if (enq_id > node_id) {
    if (
        cell.val.load(relaxed) == top_ptr<void>()
        && this->m_enq_idx.load(relaxed) <= node_id
    ) {
      return nullptr;
    }
  } else {
    if (
        (enq_id > 0 && enq->id.compare_exchange_strong(enq_id, -node_id, relaxed, relaxed))
        || (enq_id == -node_id && cell.val.load(relaxed) == top_ptr<void>())
    ) {
      auto lEi = this->m_enq_idx.load(relaxed);
      while (lEi <= node_id && !this->m_enq_idx.compare_exchange_strong(lEi, node_id + 1, relaxed, relaxed)) {}
      cell.val.store(enq_val, relaxed);
    }
  }

  return cell.val.load(relaxed);
}

/********** private methods (dequeue) *************************************************************/

void* erased_queue_t::deq_fast(handle_t& th, std::intmax_t& id) {
  // increment dequeue index
  const auto i = this->m_deq_idx.fetch_add(1, seq_cst);
  auto [cell, curr] = find_cell(th.head, th, i);
  th.head.store(&curr, relaxed);
  void* res = this->help_enq(cell, th, i);
  deq_req_t* cd = nullptr;

  if (res == nullptr) {
    return nullptr;
  }

  if (
      res != top_ptr<void>() &&
      cell.deq_req.compare_exchange_strong(cd, top_ptr<deq_req_t>(), relaxed, relaxed)
  ) {
    return res;
  }

  id = i;
  return top_ptr<void>();
}

void* erased_queue_t::deq_slow(handle_t& th, std::intmax_t id) {
  auto& deq = th.deq_req;
  deq.id.store(id, release);
  deq.idx.store(id, release);

  this->help_deq(th, th);

  const auto i = -1 * deq.idx.load(relaxed);
  auto [cell, curr] = find_cell(th.head, th, i);
  th.head.store(&curr, relaxed);
  auto res = cell.val.load(relaxed);

  return res == top_ptr<void>() ? nullptr : res;
}

void erased_queue_t::help_deq(handle_t& th, handle_t& ph) {
  auto& deq = ph.deq_req;
  auto idx = deq.idx.load(acquire);
  const auto id = deq.id.load(relaxed);

  if (idx < id) {
    return;
  }

  const auto lDp = ph.head.load(relaxed);
  const auto hzd_node_id = ph.hzd_node_id.load(relaxed);
  th.hzd_node_id.store(hzd_node_id, seq_cst);
  idx = deq.idx.load(relaxed);

  auto i = id + 1;
  auto old_val = id;
  auto new_val = 0;

  while (true) {
    for (; idx == old_val && new_val == 0; ++i) {
      auto [cell, _ignore] = find_cell(ph.head, th, i);

      auto lDi = this->m_deq_idx.load(relaxed);
      while (lDi <= i && !this->m_deq_idx.compare_exchange_weak(lDi, i + 1, relaxed, relaxed)) {}

      auto res = this->help_enq(cell, th, i);
      if (res == nullptr || (res != top_ptr<void>() && cell.deq_req.load(relaxed) == nullptr)) {
        new_val = i;
      } else {
        idx = deq.idx.load(acquire);
      }
    }

    if (new_val != 0) {
      if (deq.idx.compare_exchange_strong(idx, new_val, release, acquire)) {
        idx = new_val;
      }

      if (idx >= new_val) {
        new_val = 0;
      }
    }

    if (idx < 0 || deq.id.load(relaxed) != id) {
      break;
    }

    auto [cell, _ignore] = find_cell(ph.head, th, idx);
    deq_req_t* cd = nullptr;
    if (
        cell.val.load(relaxed) == top_ptr<void>() ||
        cell.deq_req.compare_exchange_strong(cd, &deq, relaxed, relaxed) ||
        cd == &deq
    ) {
      deq.idx.compare_exchange_strong(idx, -idx, relaxed, relaxed);
      break;
    }

    old_val = idx;
    if (idx >= i) {
      i = idx + 1;
    }
  }
}
}
