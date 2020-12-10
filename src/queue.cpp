#include "private/queue_private.hpp"

#include <atomic>
#include <stdexcept>

//#include "private/ordering.hpp"

namespace ymc::detail {
constexpr auto RELAXED = std::memory_order_relaxed;
constexpr auto ACQUIRE = std::memory_order_acquire;
constexpr auto RELEASE = std::memory_order_release;
constexpr auto SEQ_CST = std::memory_order_seq_cst;

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
  const auto hzd_node_id = peer_hzd_node_id.load(ACQUIRE);

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
  auto node = peer_node.load(ACQUIRE);

  if (node->id < curr->id) {
    if (!peer_node.compare_exchange_strong(node, curr, SEQ_CST, SEQ_CST)) {
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
  auto curr = ptr.load(RELAXED);

  for (auto j = curr->id; j < i / NODE_SIZE; ++j) {
    auto next = curr->next.load(RELAXED);

    if (next == nullptr) {
      auto tmp = th.spare_node;

      if (tmp == nullptr) {
        tmp = new node_t();
        th.spare_node = tmp;
      }

      tmp->id = j + 1;

      if (curr->next.compare_exchange_strong(next, tmp, RELEASE, ACQUIRE)) {
        next = tmp;
        th.spare_node = nullptr;
      }
    }

    curr = next;
  }

  return { curr->cells[i % NODE_SIZE], *curr };
}

/********** constructor & destructor ******************************************/

erased_queue_t::erased_queue_t(size_t max_threads):
  m_handles{}, m_max_threads(max_threads)
{
  if (max_threads == 0) {
    throw std::invalid_argument("max_threads must be at least 1");
  }

  auto node = new node_t();
  this->m_head.store(node, RELAXED);

  for (auto i = 0; i < max_threads; ++i) {
    this->m_handles.emplace_back(i, node, max_threads);
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
  auto curr = this->m_head.load(RELAXED);
  while (curr != nullptr) {
    auto tmp = curr;
    curr = curr->next.load(RELAXED);
    delete tmp;
  }

  // delete any remaining thread-local spare nodes
  for (auto& handle : this->m_handles) {
    delete handle.spare_node;
  }
}

/********** public methods ****************************************************/

void erased_queue_t::enqueue(void* elem, std::size_t thread_id) {
  auto& th = this->m_handles[thread_id];
  th.hzd_node_id.store(th.tail_node_id, RELAXED);

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

  th.tail_node_id = th.tail.load(RELAXED)->id;
  th.hzd_node_id.store(MAX_U64, RELEASE);
}

void* erased_queue_t::dequeue(std::size_t thread_id) {
  auto& th = this->m_handles[thread_id];
  th.hzd_node_id.store(th.head_node_id, RELAXED);

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

  th.head_node_id = th.head.load(RELAXED)->id;
  th.hzd_node_id.store(MAX_U64, RELEASE);

  if (th.spare_node == nullptr) {
    this->cleanup(th);
    th.spare_node = new node_t();
  }

  return res;
}

/********** private methods ***************************************************/

void erased_queue_t::cleanup(handle_t& th) {
 auto oid = this->m_help_idx.load(ACQUIRE);
 auto new_node = th.head.load(RELAXED);

 if (oid == -1) {
   return;
 }

 if (new_node->id - oid < (this->m_max_threads * 2)) {
   return;
 }

 if (
     !this->m_help_idx.compare_exchange_strong(oid, -1, ACQUIRE, RELAXED)
 ) {
   return;
 }

 auto lDi = this->m_deq_idx.load(RELAXED);
 auto lEi = this->m_enq_idx.load(RELAXED);

 while (
     lEi <= lDi &&
     !this->m_enq_idx.compare_exchange_weak(
         lEi, lDi + 1, RELAXED, RELAXED)
 ) {}

 auto old_node = this->m_head.load(RELAXED);
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
    this->m_help_idx.store(oid, RELEASE);
  } else {
    this->m_head.store(new_node, RELAXED);
    this->m_help_idx.store(nid, RELEASE);

    while (old_node != new_node) {
      auto tmp = old_node->next.load(RELAXED);
      delete old_node;
      old_node = tmp;
    }
  }
}

/********** private methods (enqueue) *****************************************/

bool erased_queue_t::enq_fast(void* elem, handle_t& th, int64_t& id) {
  const auto i = this->m_enq_idx.fetch_add(1, SEQ_CST);
  auto [cell, curr] = find_cell(th.tail, th, i);
  th.tail.store(&curr, RELAXED);

  void* cell_val = nullptr;

  if (cell.val.compare_exchange_strong(cell_val, elem, RELAXED, RELAXED)) {
    return true;
  } else {
    id = i;
    return false;
  }
}

void erased_queue_t::enq_slow(void* elem, handle_t& th, int64_t id) {
  auto& enq = th.enq_req;
  enq.val.store(elem, RELAXED);
  enq.id.store(id, RELEASE);

  int64_t i;
  do {
    i = this->m_enq_idx.fetch_add(1, RELAXED);
    auto [cell, _ignore] = find_cell(th.tail, th, i);

    enq_req_t* expected = nullptr;
    if (
        cell.enq_req.compare_exchange_strong(
            expected, &enq, SEQ_CST, SEQ_CST) &&
        cell.val.load(RELAXED) != top_ptr<enq_req_t>()
    ) {
      if (enq.id.compare_exchange_strong(id, -i, RELAXED, RELAXED)) {
        id = -i;
      }

      break;
    }
  } while (enq.id.load(RELAXED) > 0);

  id = -enq.id.load(RELAXED);
  auto [cell, curr] = find_cell(th.tail, th, id);
  th.tail.store(&curr, RELAXED);

  if (id > i) {
    auto lEi = this->m_enq_idx.load(RELAXED);
    while (
        lEi <= id &&
        !this->m_enq_idx.compare_exchange_weak(
            lEi, id + 1, RELAXED, RELAXED)
    ) {}
  }

  cell.val.store(elem, RELAXED);
}

void* erased_queue_t::help_enq(cell_t& cell, handle_t& th, int64_t i) {
  auto res = cell.val.load(ACQUIRE);

  if (res != top_ptr<void>() && res != nullptr) {
    return res;
  }

  if (
      res == nullptr &&
      !cell.val.compare_exchange_strong(
          res, top_ptr<void>(), SEQ_CST, SEQ_CST)
  ) {
    if (res != top_ptr<void>()) {
      return res;
    }
  }

  auto enq = cell.enq_req.load(RELAXED);

  if (enq == nullptr) {
    auto ph = th.enq_help_handle;
    auto pe = &ph->enq_req;
    auto id = pe->id.load(RELAXED);

    if (th.Ei != 0 && th.Ei != id) {
      th.Ei = 0;
      th.enq_help_handle = ph->next;
      ph = th.enq_help_handle;
      pe = &ph->enq_req;
      id = pe->id;
    }

    if (
        id > 0 && id <= i &&
        !cell.enq_req.compare_exchange_strong(enq, pe, RELAXED, RELAXED) &&
        enq != pe
    ) {
      th.Ei = id;
    } else {
      th.Ei = 0;
      th.enq_help_handle = ph->next;
    }

    if (
        enq == nullptr &&
        cell.enq_req.compare_exchange_strong(
            enq, top_ptr<enq_req_t>(), RELAXED, RELAXED)
    ) {
      enq = top_ptr<enq_req_t>();
    }
  }

  if (enq == top_ptr<enq_req_t>()) {
    return (this->m_enq_idx.load(RELAXED) <= i ? nullptr : top_ptr<void>());
  }

  auto enq_id = enq->id.load(ACQUIRE);
  const auto enq_val = enq->val.load(ACQUIRE);

  if (enq_id > i) {
    if (
        cell.val.load(RELAXED) == top_ptr<void>() &&
        this->m_enq_idx.load(RELAXED) <= i
    ) {
      return nullptr;
    }
  } else {
    if (
        (enq_id > 0 && enq->id.compare_exchange_strong(enq_id, -i, RELAXED, RELAXED)) ||
        (enq_id == -i && cell.val.load(RELAXED) == top_ptr<void>())
    ) {
      auto lEi = this->m_enq_idx.load(RELAXED);
      while (lEi <= i && !this->m_enq_idx.compare_exchange_strong(lEi, i + 1, RELAXED, RELAXED)) {}
      cell.val.store(enq_val, RELAXED);
    }
  }

  return cell.val.load(RELAXED);
}

/********** private methods (dequeue) *****************************************/

void* erased_queue_t::deq_fast(handle_t& th, int64_t& id) {
  // increment dequeue index
  const auto i = this->m_deq_idx.fetch_add(1, SEQ_CST);
  auto [cell, curr] = find_cell(th.head, th, i);
  th.head.store(&curr, RELAXED);
  void* res = this->help_enq(cell, th, i);
  deq_req_t* cd = nullptr;

  if (res == nullptr) {
    return nullptr;
  }

  if (
      res != top_ptr<void>() &&
      cell.deq_req.compare_exchange_strong(cd, top_ptr<deq_req_t>(), RELAXED, RELAXED)
  ) {
    return res;
  }

  id = i;
  return top_ptr<void>();
}

void* erased_queue_t::deq_slow(handle_t& th, int64_t id) {
  auto& deq = th.deq_req;
  deq.id.store(id, RELEASE);
  deq.idx.store(id, RELEASE);

  this->help_deq(th, th);

  const auto i = -1 * deq.idx.load(RELAXED);
  auto [cell, curr] = find_cell(th.head, th, i);
  th.head.store(&curr, RELAXED);
  auto res = cell.val.load(RELAXED);

  return res == top_ptr<void>() ? nullptr : res;
}

void erased_queue_t::help_deq(handle_t& th, handle_t& ph) {
  auto& deq = ph.deq_req;
  auto idx = deq.idx.load(ACQUIRE);
  const auto id = deq.id.load(RELAXED);

  if (idx < id) {
    return;
  }

  const auto lDp = ph.head.load(RELAXED);
  const auto hzd_node_id = ph.hzd_node_id.load(RELAXED);
  th.hzd_node_id.store(hzd_node_id, SEQ_CST);
  idx = deq.idx.load(RELAXED);

  auto i = id + 1;
  auto old_val = id;
  auto new_val = 0;

  while (true) {
    for (; idx == old_val && new_val == 0; ++i) {
      auto [cell, _ignore] = find_cell(ph.head, th, i);

      auto lDi = this->m_deq_idx.load(RELAXED);
      while (lDi <= i && !this->m_deq_idx.compare_exchange_weak(lDi, i + 1, RELAXED, RELAXED)) {}

      auto res = this->help_enq(cell, th, i);
      if (res == nullptr || (res != top_ptr<void>() && cell.deq_req.load(RELAXED) == nullptr)) {
        new_val = i;
      } else {
        idx = deq.idx.load(ACQUIRE);
      }
    }

    if (new_val != 0) {
      if (deq.idx.compare_exchange_strong(idx, new_val, RELEASE, ACQUIRE)) {
        idx = new_val;
      }

      if (idx >= new_val) {
        new_val = 0;
      }
    }

    if (idx < 0 || deq.id.load(RELAXED) != id) {
      break;
    }

    auto [cell, _ignore] = find_cell(ph.head, th, idx);
    deq_req_t* cd = nullptr;
    if (
        cell.val.load(RELAXED) == top_ptr<void>() ||
        cell.deq_req.compare_exchange_strong(cd, &deq, RELAXED, RELAXED) ||
        cd == &deq
    ) {
      deq.idx.compare_exchange_strong(idx, -idx, RELAXED, RELAXED);
      break;
    }

    old_val = idx;
    if (idx >= i) {
      i = idx + 1;
    }
  }
}
}
