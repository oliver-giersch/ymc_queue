#include "private/queue_private.hpp"

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
  return reinterpret_cast<T*>(std::numeric_limits<size_t>::max());
}

/** Check ... */
node_t* check(
    const std::atomic<uint64_t>& peer_hzd_node_id,
    node_t* curr,
    node_t* old
) {
  const auto hzd_node_id = peer_hzd_node_id.load(acquire);

  if (hzd_node_id < curr->id) {
    auto tmp = old;
    while (tmp->id < hzd_node_id) {
      tmp = tmp->next;
    }
    curr = tmp;
  }

  return curr;
}

/** ??? */
node_t* update(
    std::atomic<node_t*>& peer_node,
    const std::atomic<uint64_t>& peer_hzd_node_id,
    node_t* curr,
    node_t* old
) {
  auto node = peer_node.load(acquire);

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

/** Does what? */
find_cell_result_t find_cell(
    const std::atomic<node_t*>& ptr,
    handle_t& th,
    int64_t i
) {
  auto curr = ptr.load(relaxed);

  for (auto j = curr->id; j < i / NODE_SIZE; ++j) {
    auto next = curr->next.load(relaxed);

    if (next == nullptr) {
      if (th.has_appended_node) {
        th.has_appended_node = false;
      }

      auto node = new node_t();
      node->id = j + 1;

      if (curr->next.compare_exchange_strong(next, node, release, acquire)) {
        next = node;
        th.has_appended_node = true;
      } else {
        delete node;
      }
    }

    curr = next;
  }

  return { curr->cells[i % NODE_SIZE], *curr };
}

/********** constructor & destructor **************************************************************/

erased_queue_t::erased_queue_t(size_t max_threads):
  m_handles{}, m_max_threads(max_threads)
{
  if (max_threads == 0) {
    throw std::invalid_argument("max_threads must be at least 1");
  }

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
}

/********** public methods ************************************************************************/

void erased_queue_t::enqueue(void* elem, std::size_t thread_id) {
  auto& th = this->m_handles[thread_id];
  th.hzd_node_id.store(th.tail_node_id, relaxed);

  int64_t id = 0;
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
  th.hzd_node_id.store(MAX_U64, release);
}

void* erased_queue_t::dequeue(std::size_t thread_id) {
  auto& th = this->m_handles[thread_id];
  th.hzd_node_id.store(th.head_node_id, relaxed);

  int64_t id = 0;
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
  th.hzd_node_id.store(MAX_U64, release);

  if (th.has_appended_node) {
    this->cleanup(th);
    th.has_appended_node = false;
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

bool erased_queue_t::enq_fast(void* elem, handle_t& th, int64_t& id) {
  const auto i = this->m_enq_idx.fetch_add(1, seq_cst);
  auto [cell, curr] = find_cell(th.tail, th, i);
  th.tail.store(&curr, relaxed);

  void* expected = nullptr;
  if (cell.val.compare_exchange_strong(expected, elem, relaxed, relaxed)) {
    return true;
  } else {
    id = i;
    return false;
  }
}

void erased_queue_t::enq_slow(void* elem, handle_t& th, int64_t id) {
  auto& enq = th.enq_req;
  enq.val.store(elem, relaxed);
  enq.id.store(id, release);

  int64_t i;
  do {
    i = this->m_enq_idx.fetch_add(1, relaxed);
    auto [cell, _ignore] = find_cell(th.tail, th, i);

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
  auto [cell, curr] = find_cell(th.tail, th, id);
  th.tail.store(&curr, relaxed);

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

void* erased_queue_t::help_enq(cell_t& cell, handle_t& th, int64_t i) {
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
    auto ph = th.enq_help_handle;
    auto pe = &ph->enq_req;
    auto id = pe->id.load(relaxed);

    if (th.Ei != 0 && th.Ei != id) {
      th.Ei = 0;
      th.enq_help_handle = ph->next;
      ph = th.enq_help_handle;
      pe = &ph->enq_req;
      id = pe->id;
    }

    if (
        id > 0 && id <= i
        && !cell.enq_req.compare_exchange_strong(enq, pe, relaxed, relaxed)
        && enq != pe
    ) {
      th.Ei = id;
    } else {
      th.Ei = 0;
      th.enq_help_handle = ph->next;
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
    return (this->m_enq_idx.load(relaxed) <= i ? nullptr : top_ptr<void>());
  }

  auto enq_id = enq->id.load(acquire);
  const auto enq_val = enq->val.load(acquire);

  if (enq_id > i) {
    if (
        cell.val.load(relaxed) == top_ptr<void>()
        && this->m_enq_idx.load(relaxed) <= i
    ) {
      return nullptr;
    }
  } else {
    if (
        (enq_id > 0 && enq->id.compare_exchange_strong(enq_id, -i, relaxed, relaxed))
        || (enq_id == -i && cell.val.load(relaxed) == top_ptr<void>())
    ) {
      auto lEi = this->m_enq_idx.load(relaxed);
      while (lEi <= i && !this->m_enq_idx.compare_exchange_strong(lEi, i + 1, relaxed, relaxed)) {}
      cell.val.store(enq_val, relaxed);
    }
  }

  return cell.val.load(relaxed);
}

/********** private methods (dequeue) *************************************************************/

void* erased_queue_t::deq_fast(handle_t& th, int64_t& id) {
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

void* erased_queue_t::deq_slow(handle_t& th, int64_t id) {
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
