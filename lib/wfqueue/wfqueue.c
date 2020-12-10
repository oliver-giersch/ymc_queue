#include "wfqueue.h"

#include <stdlib.h>
#include <string.h>

#include "primitives.h"

#define N WFQUEUE_NODE_SIZE
#define NULLPTR ((void *)0)
#define TOP ((void *)-1)

#define MAX_GARBAGE(n) (2 * n)

#ifndef MAX_SPIN
#define MAX_SPIN 100
#endif

#ifndef MAX_PATIENCE
#define MAX_PATIENCE 10
#endif

typedef struct wfq_enq_t    enq_t;
typedef struct wfq_deq_t    deq_t;
typedef struct wfq_cell_t   cell_t;
typedef struct wfq_node_t   node_t;
typedef struct wfq_queue_t  queue_t;
typedef struct wfq_handle_t handle_t;

static void cleanup(queue_t *q, handle_t *th);

static int enq_fast(queue_t *q, handle_t *th, void *v, long *id);
static void enq_slow(queue_t *q, handle_t *th, void *v, long id);
static void *help_enq(queue_t *q, handle_t *th, cell_t *c, long i);

static void *deq_fast(queue_t *q, handle_t *th, long *id);
static void *deq_slow(queue_t *q, handle_t *th, long id);
static void help_deq(queue_t *q, handle_t *th, handle_t *ph);

static inline void *spin(void *volatile *p) {
  int patience = MAX_SPIN;
  void *v = *p;

  while (!v && patience-- > 0) {
    v = *p;
    PAUSE();
  }

  return v;
}

node_t *new_node() {
  node_t *n = align_malloc(PAGE_SIZE, sizeof(node_t));
  memset(n, 0, sizeof(node_t));
  return n;
}

static node_t *check(
    const unsigned long volatile *p_hzd_node_id,
    node_t *cur,
    node_t *old
) {
  unsigned long hzd_node_id = ACQUIRE(p_hzd_node_id);

  if (hzd_node_id < cur->id) {
    node_t *tmp = old;
    while (tmp->id < hzd_node_id) {
      tmp = tmp->next;
    }
    cur = tmp;
  }

  return cur;
}

static node_t *update(
    node_t *volatile *pPn,
    node_t *cur,
    const unsigned long volatile *p_hzd_node_id,
    node_t *old
) {
  node_t *ptr = ACQUIRE(pPn);

  if (ptr->id < cur->id) {
    if (!CAScs(pPn, &ptr, cur)) {
      if (ptr->id < cur->id) cur = ptr;
    }

    cur = check(p_hzd_node_id, cur, old);
  }

  return cur;
}

static cell_t *find_cell(node_t *volatile *ptr, long i, handle_t *th) {
  node_t *curr = *ptr;

  long j;
  for (j = curr->id; j < i / N; ++j) {
    node_t *next = curr->next;

    if (next == NULL) {
      node_t *temp = th->spare;

      if (!temp) {
        temp = new_node();
        th->spare = temp;
      }

      temp->id = j + 1;

      if (CASra(&curr->next, &next, temp)) {
        next = temp;
        th->spare = NULL;
      }
    }

    curr = next;
  }

  *ptr = curr;
  return &curr->cells[i % N];
}

/********** public functions **************************************************/

void enqueue(queue_t *q, handle_t *th, void *v) {
  th->hzd_node_id = th->enq_node_id;

  long id;
  int p = MAX_PATIENCE;
  while (!enq_fast(q, th, v, &id) && p-- > 0)
    ;
  if (p < 0) enq_slow(q, th, v, id);

  th->enq_node_id = th->Ep->id;
  RELEASE(&th->hzd_node_id, -1);
}

void *dequeue(queue_t *q, handle_t *th) {
  th->hzd_node_id = th->deq_node_id;

  void *v;
  long id = 0;
  int p = MAX_PATIENCE;

  do
    v = deq_fast(q, th, &id);
  while (v == TOP && p-- > 0);
  if (v == TOP)
    v = deq_slow(q, th, id);
  else {
#ifdef RECORD
    th->fastdeq++;
#endif
  }

  if (v != EMPTY) {
    help_deq(q, th, th->Dh);
    th->Dh = th->Dh->next;
  }

  th->deq_node_id = th->Dp->id;
  RELEASE(&th->hzd_node_id, -1);

  if (th->spare == NULL) {
    cleanup(q, th);
    th->spare = new_node();
  }

#ifdef RECORD
  if (v == EMPTY) th->empty++;
#endif
  return v;
}

/********** private functions *************************************************/

static void cleanup(queue_t *q, handle_t *th) {
  long oid = ACQUIRE(&q->Hi);
  node_t *new = th->Dp;

  if (oid == -1) return;
  if (new->id - oid < MAX_GARBAGE(q->nprocs)) return;
  if (!CASa(&q->Hi, &oid, -1)) return;

  long Di = q->Di, Ei = q->Ei;
  while(Ei <= Di && !CAS(&q->Ei, &Ei, Di + 1))
    ;

  node_t *old = q->Hp;
  handle_t *ph = th;
  handle_t *phs[q->nprocs];
  int i = 0;

  do {
    new = check(&ph->hzd_node_id, new, old);
    new = update(&ph->Ep, new, &ph->hzd_node_id, old);
    new = update(&ph->Dp, new, &ph->hzd_node_id, old);

    phs[i++] = ph;
    ph = ph->next;
  } while (new->id > oid && ph != th);

  while (new->id > oid && --i >= 0) {
    new = check(&phs[i]->hzd_node_id, new, old);
  }

  long nid = new->id;

  if (nid <= oid) {
    RELEASE(&q->Hi, oid);
  } else {
    q->Hp = new;
    RELEASE(&q->Hi, nid);

    while (old != new) {
      node_t *tmp = old->next;
      free(old);
      old = tmp;
    }
  }
}

/********** private functions (enqueue) ***************************************/

static int enq_fast(queue_t *q, handle_t *th, void *v, long *id) {
  long i = FAAcs(&q->Ei, 1);
  cell_t *c = find_cell(&th->Ep, i, th);
  void *cv = NULLPTR;

  if (CAS(&c->val, &cv, v)) {
#ifdef RECORD
    th->fastenq++;
#endif
    return 1;
  } else {
    *id = i;
    return 0;
  }
}

static void enq_slow(queue_t *q, handle_t *th, void *v, long id) {
  enq_t *enq = &th->Er;
  enq->val = v;
  RELEASE(&enq->id, id);

  node_t *tail = th->Ep;
  long i;
  cell_t *c;

  do {
    i = FAA(&q->Ei, 1);
    c = find_cell(&tail, i, th);
    enq_t *ce = NULLPTR;

    if (CAScs(&c->enq, &ce, enq) && c->val != TOP) {
      if (CAS(&enq->id, &id, -i)) id = -i;
      break;
    }
  } while (enq->id > 0);

  id = -enq->id;
  c = find_cell(&th->Ep, id, th);
  if (id > i) {
    long Ei = q->Ei;
    while (Ei <= id && !CAS(&q->Ei, &Ei, id + 1))
      ;
  }
  c->val = v;

#ifdef RECORD
  th->slowenq++;
#endif
}

static void *help_enq(queue_t *q, handle_t *th, cell_t *c, long i) {
  void *v = spin(&c->val);

  if ((v != TOP && v != NULLPTR) ||
      (v == NULLPTR && !CAScs(&c->val, &v, TOP) && v != TOP)) {
    return v;
  }

  enq_t *e = c->enq;

  if (e == NULLPTR) {
    handle_t *ph;
    enq_t *pe;
    long id;
    ph = th->Eh, pe = &ph->Er, id = pe->id;

    if (th->Ei != 0 && th->Ei != id) {
      th->Ei = 0;
      th->Eh = ph->next;
      ph = th->Eh, pe = &ph->Er, id = pe->id;
    }

    if (id > 0 && id <= i && !CAS(&c->enq, &e, pe) && e != pe)
      th->Ei = id;
    else {
      th->Ei = 0;
      th->Eh = ph->next;
    }

    if (e == NULLPTR && CAS(&c->enq, &e, TOP)) e = TOP;
  }

  if (e == TOP) return (q->Ei <= i ? NULLPTR : TOP);

  long ei = ACQUIRE(&e->id);
  void *ev = ACQUIRE(&e->val);

  if (ei > i) {
    if (c->val == TOP && q->Ei <= i) return NULLPTR;
  } else {
    if ((ei > 0 && CAS(&e->id, &ei, -i)) || (ei == -i && c->val == TOP)) {
      long Ei = q->Ei;
      while (Ei <= i && !CAS(&q->Ei, &Ei, i + 1))
        ;
      c->val = ev;
    }
  }

  return c->val;
}

/********** private functions (dequeue) ***************************************/

static void *deq_fast(queue_t *q, handle_t *th, long *id) {
  long i = FAAcs(&q->Di, 1);
  cell_t *c = find_cell(&th->Dp, i, th);
  void *v = help_enq(q, th, c, i);
  deq_t *cd = NULLPTR;

  if (v == NULLPTR) return NULLPTR;
  if (v != TOP && CAS(&c->deq, &cd, TOP)) return v;

  *id = i;
  return TOP;
}

static void *deq_slow(queue_t *q, handle_t *th, long id) {
  deq_t *deq = &th->Dr;
  RELEASE(&deq->id, id);
  RELEASE(&deq->idx, id);

  help_deq(q, th, th);
  long i = -deq->idx;
  cell_t *c = find_cell(&th->Dp, i, th);
  void *val = c->val;

#ifdef RECORD
  th->slowdeq++;
#endif
  return val == TOP ? NULLPTR : val;
}

static void help_deq(queue_t *q, handle_t *th, handle_t *ph) {
  deq_t *deq = &ph->Dr;
  long idx = ACQUIRE(&deq->idx);
  long id = deq->id;

  if (idx < id) return;

  // typeof Dp = volatile node_t*
  node_t *Dp = ph->Dp; // <- copies (loads) the volatile pointer!!
  th->hzd_node_id = ph->hzd_node_id;
  FENCE();
  idx = deq->idx;

  long i = id + 1, old = id, new = 0;
  while (1) {
    node_t *h = Dp;
    for (; idx == old && new == 0; ++i) {
      cell_t *c = find_cell(&h, i, th);

      long Di = q->Di;
      while (Di <= i && !CAS(&q->Di, &Di, i + 1))
        ;

      void *v = help_enq(q, th, c, i);
      if (v == NULLPTR || (v != TOP && c->deq == NULLPTR))
        new = i;
      else
        idx = ACQUIRE(&deq->idx);
    }

    if (new != 0) {
      if (CASra(&deq->idx, &idx, new)) idx = new;
      if (idx >= new) new = 0;
    }

    if (idx < 0 || deq->id != id) break;

    cell_t *c = find_cell(&Dp, idx, th); // <- likewise only modifies the stack local variable
    deq_t *cd = NULLPTR;
    if (c->val == TOP || CAS(&c->deq, &cd, deq) || cd == deq) {
      CAS(&deq->idx, &idx, -idx);
      break;
    }

    old = idx;
    if (idx >= i) i = idx + 1;
  }
}
