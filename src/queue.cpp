#include <stdexcept>
#include "private/queue_private.hpp"

#include "private/ordering.hpp"

namespace ymc::detail {
template<typename T>
constexpr T* top_ptr() {
  return reinterpret_cast<T*>(std::numeric_limits<size_t>::max());
}

node_t* check(
    std::atomic<uint64_t>& p_hzd_node_id,
    node_t* curr,
    node_t* old
) {
  const auto hzd_node_id = p_hzd_node_id.load(ACQ);
  if (hzd_node_id < curr->id) {
    auto tmp = old;
    while (tmp->id < hzd_node_id) {
      tmp = tmp->next;
    }
    curr = tmp;
  }

  return curr;
}

node_t* update(
    std::atomic<node_t*>& pPn,
    node_t* curr,
    std::atomic<uint64_t>& p_hzd_node_id,
    node_t* old
) {
  auto peer_node = pPn.load(ACQ);
  if (peer_node->id < curr->id) {
    if (!pPn.compare_exchange_strong(peer_node, curr)) {
      if (peer_node->id < curr->id) {
        curr = peer_node;
      }
    }

    curr = check(p_hzd_node_id, curr, old);
  }

  return curr;
}

/** Does what? */
cell_t& find_cell(std::atomic<node_t*>& ptr, handle_t& th, int64_t i) {
  auto curr = ptr.load(RLX);

  for (auto j = curr->id; j < i / NODE_SIZE; ++j) {
    auto next = curr->next.load(RLX);

    if (next == nullptr) {
      auto tmp = th.spare;

      if (tmp == nullptr) {
        tmp = new node_t();
        th.spare = tmp;
      }

      tmp->id = j + 1;

      if (curr->next.compare_exchange_strong(next, tmp, REL_ACQ_CAS)) {
        next = tmp;
        th.spare = nullptr;
      }
    }

    curr = next;
  }

  ptr.store(curr, RLX);
  return curr->cells[i % NODE_SIZE];
}

/********** constructor & destructor ******************************************/

erased_queue_t::erased_queue_t(size_t max_threads):
  m_handles{}, m_max_threads(max_threads)
{
  if (max_threads == 0) {
    throw std::invalid_argument("max_threads must be at least 1");
  }

  auto node = new node_t();
  this->Hp.store(node, RLX);

  for (auto i = 0; i < max_threads; ++i) {
    this->m_handles.emplace_back(node, max_threads);
  }

  for (auto i = 0; auto& handle : this->m_handles) {
    auto next = i == max_threads - 1 ? &this->m_handles[0] : &this->m_handles[i + 1];
    handle.next = next;
    handle.Eh = next;
    handle.Dh = next;
  }
}

erased_queue_t::~erased_queue_t() noexcept {
  // delete all remaining nodes in the queue
  auto curr = this->Hp.load(RLX);
  while (curr != nullptr) {
    auto tmp = curr;
    curr = curr->next.load(RLX);
    delete tmp;
  }

  // delete any remaining thread-local spare nodes
  for (auto& handle : this->m_handles) {
    if (handle.spare != nullptr) {
      delete handle.spare;
    }
  }
}

/********** public methods ****************************************************/

void erased_queue_t::enqueue(void* v, std::size_t thread_id) {
  auto& th = this->m_handles[thread_id];
  th.hzd_node_id = th.enq_node_id;

  int64_t id = 0;
  bool success = false;
  for (auto patience = 0; patience < PATIENCE; ++patience) {
    if ((success = this->enq_fast(v, th, id))) {
      break;
    }
  }

  if (!success) {
    this->enq_slow(v, th, id);
  }

  th.enq_node_id = th.Ep.load(RLX)->id;
  th.hzd_node_id.store(MAX_U64, REL);
}

void* erased_queue_t::dequeue(std::size_t thread_id) {
  auto& th = this->m_handles[thread_id];
  th.hzd_node_id = th.deq_node_id;

  int64_t id = 0;
  void* res = nullptr;

  for (auto patience = 0; patience < PATIENCE; ++patience) {
    res = this->deq_fast(th, id);
    if (res != top_ptr<void>()) {
      break;
    }
  }

  if (res == top_ptr<void>()) {
    res = this->deq_slow(th, id);
  }

  if (res != nullptr) {
    this->help_deq(th, *th.Dh);
    th.Dh = th.Dh->next;
  }

  th.deq_node_id = th.Dp.load(RLX)->id;
  th.hzd_node_id.store(MAX_U64, REL);

  if (th.spare == nullptr) {
    this->cleanup(th);
    th.spare = new node_t();
  }

  return res;
}

/********** private methods ***************************************************/

void erased_queue_t::cleanup(handle_t& th) {
 auto oid = this->Hi.load(ACQ);
 auto new_node = th.Dp.load(RLX);

 if (oid == -1) {
   return;
 }

 if (new_node->id - oid < this->m_max_threads * 2) {
   return;
 }

 if (!this->Hi.compare_exchange_strong(oid, -1, ACQ_CAS)) {
   return;
 }

 auto lDi = this->Di.load(RLX);
 auto lEi = this->Ei.load(RLX);

 while (lEi <= lDi && !this->Ei.compare_exchange_weak(lEi, lDi + 1, RLX_CAS)) {}

 auto old_node = this->Hp.load(RLX);
 auto ph = &th;
 auto i = 0;

 for (auto& handle : th.handles) {
   new_node = check(ph->hzd_node_id, new_node, old_node);
   new_node = update(ph->Ep, new_node, ph->hzd_node_id, old_node);
   new_node = update(ph->Dp, new_node, ph->hzd_node_id, old_node);

   handle = ph;
   ph = ph->next;

   i += 1;

   if (!(new_node->id > oid && ph != &th)) {
     break;
   }
 }

 while (new_node->id > oid && i > 0) {
   i -= 1;
   new_node = check(th.handles[i]->hzd_node_id, new_node, old_node);
 }

 const auto nid = new_node->id;

  if (nid <= oid) {
    this->Hi.store(oid, REL);
  } else {
    this->Hp.store(new_node, RLX);
    this->Hi.store(nid, REL);

    while (old_node != new_node) {
      auto tmp = old_node->next.load(RLX);
      delete old_node;
      old_node = tmp;
    }
  }
}

/********** private methods (enqueue) *****************************************/

bool erased_queue_t::enq_fast(void* v, handle_t& th, int64_t& id) {
  const auto i = this->Ei.fetch_add(1, std::memory_order_seq_cst);
  auto& cell = find_cell(th.Ep, th, i);
  void* cell_val = nullptr;

  if (cell.val.compare_exchange_strong(cell_val, v, RLX_CAS)) {
    return true;
  } else {
    id = i;
    return false;
  }
}

void erased_queue_t::enq_slow(void* elem, handle_t& th, int64_t id) {
  auto& enq = th.Er;
  enq.val.store(elem, RLX);
  enq.id.store(id, REL);

  auto& tail = th.Ep;
  int64_t i;

  do {
    i = this->Ei.fetch_add(1, RLX);
    auto& cell = find_cell(tail, th, i);
    enq_req_t* expected = nullptr;

    if (cell.enq.compare_exchange_strong(expected, &enq) && cell.val.load(RLX) != top_ptr<enq_req_t>()) {
      if (enq.id.compare_exchange_strong(id, -1, RLX_CAS)) {
        id = -1;
      }

      break;
    }
  } while (enq.id.load(RLX) > 0);

  id = -1 * enq.id.load(RLX);
  auto& cell = find_cell(th.Ep, th, id);
  if (id > i) {
    auto lEi = this->Ei.load(RLX);
    while (lEi <= id && !this->Ei.compare_exchange_weak(lEi, id + 1, RLX_CAS));
  }

  cell.val.store(elem, RLX);
}

void* erased_queue_t::help_enq(cell_t& cell, handle_t& th, int64_t i) {
  auto res = cell.val.load(RLX);

  if (res != top_ptr<void>() && res != nullptr) {
    return res;
  }

  if (res == nullptr && !cell.val.compare_exchange_strong(res, top_ptr<void>()) && res != top_ptr<void>()) {
    return res;
  }

  auto enq = cell.enq.load(RLX);

  if (enq == nullptr) {
    auto ph = th.Eh;
    auto pe = &ph->Er;
    auto id = pe->id.load(RLX);

    const auto lEi = th.Ei.load(RLX);
    if (lEi != 0 && lEi == id) {
      th.Ei.store(0, RLX);
      th.Eh = ph->next;
      ph = th.Eh;
      pe = &ph->Er;
      id = pe->id;
    }

    if (id > 0 && id <= i && !cell.enq.compare_exchange_strong(enq, pe, RLX_CAS) && enq != pe) {
      th.Ei.store(id, std::memory_order_relaxed);
    } else {
      th.Ei.store(0, std::memory_order_relaxed);
      th.Eh = ph->next;
    }

    if (enq == nullptr && cell.enq.compare_exchange_strong(enq, top_ptr<enq_req_t>(), RLX_CAS)) {
      enq = top_ptr<enq_req_t>();
    }
  }

  if (enq == top_ptr<enq_req_t>()) {
    return (this->Ei.load(RLX) <= i ? nullptr : top_ptr<void>());
  }

  auto ei = enq->id.load(ACQ);
  const auto enq_val = enq->val.load(ACQ);

  if (ei > i) {
    if (cell.val.load(RLX) == top_ptr<void>() && this->Ei.load(RLX) <= i) {
      return nullptr;
    }
  } else {
    if ((ei > 0 && enq->id.compare_exchange_strong(ei, -i, RLX_CAS)) || (ei == -i && cell.val.load(RLX) == top_ptr<void>())) {
      auto lEi = this->Ei.load(RLX);
      while (lEi <= i && !this->Ei.compare_exchange_strong(lEi, i + 1, RLX_CAS));
      cell.val.store(enq_val, RLX);
    }
  }

  return cell.val.load(RLX);
}

/********** private methods (dequeue) *****************************************/

void* erased_queue_t::deq_fast(handle_t& th, int64_t& id) {
  // increment dequeue index
  const auto i = this->Di.fetch_add(1);
  auto& cell = find_cell(th.Dp, th, i);
  void* res = this->help_enq(cell, th, i);
  deq_req_t* cd = nullptr;

  if (res == top_ptr<void>()) {
    return nullptr;
  }

  if (res != top_ptr<void>() && cell.deq.compare_exchange_strong(cd, top_ptr<deq_req_t>(), RLX_CAS)) {
    return res;
  }

  id = i;
  return top_ptr<void>();
}

void* erased_queue_t::deq_slow(handle_t& th, int64_t id) {
  auto& deq = th.Dr;
  deq.id.store(id, REL);
  deq.idx.store(id, REL);

  this->help_deq(th, th);

  auto i = -1 * deq.idx.load(RLX);
  auto& cell = find_cell(th.Dp, th, i);
  void* val = cell.val.load(RLX);

  return val == top_ptr<void>() ? nullptr : val;
}

void erased_queue_t::help_deq(handle_t& th, handle_t& ph) {
  auto& deq = ph.Dr;
  auto idx = deq.idx.load(std::memory_order_acquire);
  const auto id = deq.id.load(std::memory_order_relaxed);

  if (idx < id) return;

  auto& lDp = ph.Dp;
  const auto hzd_node_id = ph.hzd_node_id.load(std::memory_order_relaxed);
  th.hzd_node_id.store(hzd_node_id, std::memory_order_seq_cst);

  idx = deq.idx.load(std::memory_order_relaxed);
  auto i = id + 1;
  auto old_val = id;
  auto new_val = 0;

  while (true) {
    auto& h = lDp;
    for (; idx == old_val && new_val == 0; ++i) {
      auto& cell = find_cell(h, th, i);
      auto lDi = this->Di.load(std::memory_order_relaxed);
      while (lDi <= i && !this->Di.compare_exchange_weak(lDi, i + 1, RLX_CAS));

      auto v = this->help_enq(cell, th, i);
      if (v == nullptr || (v != top_ptr<void>() && cell.deq == nullptr)) {
        new_val = i;
      } else {
        idx = deq.idx.load(std::memory_order_acquire);
      }
    }

    if (new_val != 0) {
      if (deq.idx.compare_exchange_strong(idx, new_val, REL_ACQ_CAS)) {
        idx = new_val; // todo: WTF???
      }

      if (idx >= 0) {
        new_val = 0;
      }
    }

    if (idx < 0 || deq.id.load(std::memory_order_relaxed) != id) {
      break;
    }

    auto& cell = find_cell(lDp, th, idx);
    deq_req_t* cd = nullptr;
    if (cell.val == top_ptr<void>() || cell.deq.compare_exchange_strong(cd, &deq, RLX_CAS) || cd == &deq) {
      deq.idx.compare_exchange_strong(idx, -idx);
      break;
    }

    old_val = idx;
    if (idx >= 1) {
      i = idx + 1;
    }
  }
}
}
