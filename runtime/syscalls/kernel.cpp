// LV2 synchronisation objects on host primitives.
//
// One global lock guards every object; each object has its own condition
// variable. Simple and obviously correct, which matters more than speed
// until the game actually runs.
#include "kernel.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

namespace kern {
namespace {
std::mutex g;
using Lock = std::unique_lock<std::mutex>;

struct Obj {
    virtual ~Obj() = default;
    std::condition_variable cv;
};
struct Mutex : Obj { uint32_t owner = 0; int count = 0; bool recursive = false; };
// Waiters queue in arrival order; a signal marks the oldest unsignaled one,
// so a wakeup can't be lost to a waiter that is still on its way out.
struct Cond : Obj { uint32_t mutex; std::deque<bool *> waiters; };
struct Sema : Obj { int32_t value, max; };
struct EFlag : Obj { uint64_t bits; };
struct EQueue : Obj { std::deque<Event> q; size_t cap; uint64_t key = 0; bool dead = false; };
struct EPort : Obj { uint64_t name; uint32_t queue = 0; };
struct RwLock : Obj { int readers = 0; uint32_t writer = 0; };

std::map<uint32_t, std::unique_ptr<Obj>> g_objs;
uint32_t g_next = 0x40000100;

template <class T> T *get(uint32_t id) {
    auto it = g_objs.find(id);
    return it == g_objs.end() ? nullptr : dynamic_cast<T *>(it->second.get());
}
template <class T> uint32_t add(T *o) {
    uint32_t id = g_next++;
    g_objs[id].reset(o);
    return id;
}

// Wait on cv until pred() with an optional timeout (usec). Returns false on timeout.
template <class P> bool wait(Lock &lk, std::condition_variable &cv, uint64_t timeout, P pred) {
    if (!timeout) { cv.wait(lk, pred); return true; }
    return cv.wait_for(lk, std::chrono::microseconds(timeout), pred);
}
uint32_t me() { return thread::current_id(); }

int32_t lock_mutex(Lock &lk, Mutex *m, uint64_t timeout) {
    if (m->owner == me()) {
        if (!m->recursive) return EDEADLK;
        m->count++;
        return OK;
    }
    if (!wait(lk, m->cv, timeout, [&] { return m->owner == 0; })) return ETIMEDOUT;
    m->owner = me();
    m->count = 1;
    return OK;
}
} // namespace

/* ---- mutex ---- */
int32_t mutex_create(uint32_t *id, bool recursive) {
    Lock lk(g);
    auto *m = new Mutex();
    m->recursive = recursive;
    *id = add(m);
    return OK;
}
int32_t mutex_destroy(uint32_t id) { Lock lk(g); return g_objs.erase(id) ? OK : ESRCH; }
int32_t mutex_lock(uint32_t id, uint64_t timeout) {
    Lock lk(g);
    Mutex *m = get<Mutex>(id);
    return m ? lock_mutex(lk, m, timeout) : ESRCH;
}
int32_t mutex_trylock(uint32_t id) {
    Lock lk(g);
    Mutex *m = get<Mutex>(id);
    if (!m) return ESRCH;
    if (m->owner && m->owner != me()) return EBUSY;
    if (m->owner == me() && !m->recursive) return EDEADLK;
    m->owner = me();
    m->count++;
    return OK;
}
int32_t mutex_unlock(uint32_t id) {
    Lock lk(g);
    Mutex *m = get<Mutex>(id);
    if (!m) return ESRCH;
    if (m->owner != me()) return EPERM;
    if (--m->count == 0) { m->owner = 0; m->cv.notify_all(); }
    return OK;
}

/* ---- condition variable ---- */
int32_t cond_create(uint32_t *id, uint32_t mutex_id) {
    Lock lk(g);
    if (!get<Mutex>(mutex_id)) return ESRCH;
    auto *c = new Cond();
    c->mutex = mutex_id;
    *id = add(c);
    return OK;
}
int32_t cond_destroy(uint32_t id) { Lock lk(g); return g_objs.erase(id) ? OK : ESRCH; }
int32_t cond_wait(uint32_t id, uint64_t timeout) {
    Lock lk(g);
    Cond *c = get<Cond>(id);
    if (!c) return ESRCH;
    Mutex *m = get<Mutex>(c->mutex);
    if (!m || m->owner != me()) return EPERM;
    int saved = m->count;
    m->owner = 0, m->count = 0;
    m->cv.notify_all();
    bool signaled = false;
    c->waiters.push_back(&signaled);
    wait(lk, c->cv, timeout, [&] { return signaled; });
    if (!signaled) c->waiters.erase(std::find(c->waiters.begin(), c->waiters.end(), &signaled));
    wait(lk, m->cv, 0, [&] { return m->owner == 0; });  // reacquire even on timeout
    m->owner = me(), m->count = saved;
    return signaled ? OK : ETIMEDOUT;
}
int32_t cond_signal(uint32_t id, bool all) {
    Lock lk(g);
    Cond *c = get<Cond>(id);
    if (!c) return ESRCH;
    if (!c->waiters.empty()) {
        do {
            *c->waiters.front() = true;
            c->waiters.pop_front();
        } while (all && !c->waiters.empty());
        c->cv.notify_all();
    }
    return OK;
}

/* ---- semaphore ---- */
int32_t sema_create(uint32_t *id, int32_t initial, int32_t max) {
    Lock lk(g);
    auto *s = new Sema();
    s->value = initial, s->max = max;
    *id = add(s);
    return OK;
}
int32_t sema_destroy(uint32_t id) { Lock lk(g); return g_objs.erase(id) ? OK : ESRCH; }
int32_t sema_wait(uint32_t id, uint64_t timeout) {
    Lock lk(g);
    Sema *s = get<Sema>(id);
    if (!s) return ESRCH;
    if (!wait(lk, s->cv, timeout, [&] { return s->value > 0; })) return ETIMEDOUT;
    s->value--;
    return OK;
}
int32_t sema_trywait(uint32_t id) {
    Lock lk(g);
    Sema *s = get<Sema>(id);
    if (!s) return ESRCH;
    if (s->value <= 0) return EBUSY;
    s->value--;
    return OK;
}
int32_t sema_post(uint32_t id, int32_t count) {
    Lock lk(g);
    Sema *s = get<Sema>(id);
    if (!s) return ESRCH;
    if (s->value + count > s->max) return EBUSY;
    s->value += count;
    s->cv.notify_all();
    return OK;
}
int32_t sema_get_value(uint32_t id, int32_t *out) {
    Lock lk(g);
    Sema *s = get<Sema>(id);
    if (!s) return ESRCH;
    *out = s->value;
    return OK;
}

/* ---- event flag ---- */
namespace {
constexpr uint32_t EF_AND = 1, EF_OR = 2, EF_CLEAR = 0x10, EF_CLEAR_ALL = 0x20;
bool ef_match(uint64_t have, uint64_t want, uint32_t mode) {
    return (mode & EF_OR) ? (have & want) != 0 : (have & want) == want;
}
void ef_consume(EFlag *f, uint64_t want, uint32_t mode) {
    if (mode & EF_CLEAR) f->bits &= ~want;
    else if (mode & EF_CLEAR_ALL) f->bits = 0;
}
} // namespace
int32_t eflag_create(uint32_t *id, uint64_t init) {
    Lock lk(g);
    auto *f = new EFlag();
    f->bits = init;
    *id = add(f);
    return OK;
}
int32_t eflag_destroy(uint32_t id) { Lock lk(g); return g_objs.erase(id) ? OK : ESRCH; }
int32_t eflag_wait(uint32_t id, uint64_t bits, uint32_t mode, uint64_t *result, uint64_t timeout) {
    Lock lk(g);
    EFlag *f = get<EFlag>(id);
    if (!f) return ESRCH;
    if (!wait(lk, f->cv, timeout, [&] { return ef_match(f->bits, bits, mode); })) {
        if (result) *result = f->bits;
        return ETIMEDOUT;
    }
    if (result) *result = f->bits;
    ef_consume(f, bits, mode);
    return OK;
}
int32_t eflag_trywait(uint32_t id, uint64_t bits, uint32_t mode, uint64_t *result) {
    Lock lk(g);
    EFlag *f = get<EFlag>(id);
    if (!f) return ESRCH;
    if (result) *result = f->bits;
    if (!ef_match(f->bits, bits, mode)) return EBUSY;
    ef_consume(f, bits, mode);
    return OK;
}
int32_t eflag_set(uint32_t id, uint64_t bits) {
    Lock lk(g);
    EFlag *f = get<EFlag>(id);
    if (!f) return ESRCH;
    f->bits |= bits;
    f->cv.notify_all();
    return OK;
}
int32_t eflag_clear(uint32_t id, uint64_t bits) {
    Lock lk(g);
    EFlag *f = get<EFlag>(id);
    if (!f) return ESRCH;
    f->bits &= bits;  // LV2 semantics: "clear" ANDs with the given mask
    return OK;
}
int32_t eflag_get(uint32_t id, uint64_t *out) {
    Lock lk(g);
    EFlag *f = get<EFlag>(id);
    if (!f) return ESRCH;
    *out = f->bits;
    return OK;
}

/* ---- event queue / port ---- */
int32_t equeue_create(uint32_t *id, uint64_t key, int size) {
    Lock lk(g);
    auto *q = new EQueue();
    q->cap = size > 0 ? size : 127;
    q->key = key;
    *id = add(q);
    return OK;
}
int32_t equeue_destroy(uint32_t id) {
    Lock lk(g);
    EQueue *q = get<EQueue>(id);
    if (!q) return ESRCH;
    // Kept allocated (marked dead) so threads blocked in receive can wake safely.
    q->dead = true;
    q->cv.notify_all();
    return OK;
}
int32_t equeue_receive(uint32_t id, Event *ev, uint64_t timeout) {
    Lock lk(g);
    EQueue *q = get<EQueue>(id);
    if (!q || q->dead) return ESRCH;
    bool ok = wait(lk, q->cv, timeout, [&] { return q->dead || !q->q.empty(); });
    if (q->dead) return ECANCELED;
    if (!ok) return ETIMEDOUT;
    *ev = q->q.front();
    q->q.pop_front();
    return OK;
}
int32_t equeue_tryreceive(uint32_t id, Event *evs, int max, int *count) {
    Lock lk(g);
    EQueue *q = get<EQueue>(id);
    if (!q) return ESRCH;
    int n = 0;
    while (n < max && !q->q.empty()) { evs[n++] = q->q.front(); q->q.pop_front(); }
    *count = n;
    return OK;
}
int32_t equeue_drain(uint32_t id) {
    Lock lk(g);
    EQueue *q = get<EQueue>(id);
    if (!q) return ESRCH;
    q->q.clear();
    return OK;
}
int32_t equeue_push(uint32_t id, const Event &ev) {
    Lock lk(g);
    EQueue *q = get<EQueue>(id);
    if (!q) return ESRCH;
    if (q->q.size() >= q->cap) return EBUSY;
    q->q.push_back(ev);
    q->cv.notify_all();
    return OK;
}
int32_t eport_create(uint32_t *id, int type, uint64_t name) {
    Lock lk(g);
    auto *p = new EPort();
    p->name = name;
    *id = add(p);
    if (!name) p->name = ((uint64_t)0x01000000 << 32) | *id; // local port default name
    return OK;
}
int32_t eport_destroy(uint32_t id) { Lock lk(g); return g_objs.erase(id) ? OK : ESRCH; }
int32_t eport_connect(uint32_t port, uint32_t queue) {
    Lock lk(g);
    EPort *p = get<EPort>(port);
    if (!p || !get<EQueue>(queue)) return ESRCH;
    if (p->queue) return EISCONN;
    p->queue = queue;
    return OK;
}
int32_t eport_disconnect(uint32_t port) {
    Lock lk(g);
    EPort *p = get<EPort>(port);
    if (!p) return ESRCH;
    if (!p->queue) return ENOTCONN;
    p->queue = 0;
    return OK;
}
int32_t eport_send(uint32_t port, uint64_t d1, uint64_t d2, uint64_t d3) {
    uint32_t q;
    uint64_t name;
    {
        Lock lk(g);
        EPort *p = get<EPort>(port);
        if (!p) return ESRCH;
        if (!p->queue) return ENOTCONN;
        q = p->queue, name = p->name;
    }
    return equeue_push(q, {name, d1, d2, d3});
}

/* ---- rwlock ---- */
int32_t rwlock_create(uint32_t *id) { Lock lk(g); *id = add(new RwLock()); return OK; }
int32_t rwlock_destroy(uint32_t id) { Lock lk(g); return g_objs.erase(id) ? OK : ESRCH; }
int32_t rwlock_rlock(uint32_t id, uint64_t timeout) {
    Lock lk(g);
    RwLock *r = get<RwLock>(id);
    if (!r) return ESRCH;
    if (!wait(lk, r->cv, timeout, [&] { return r->writer == 0; })) return ETIMEDOUT;
    r->readers++;
    return OK;
}
int32_t rwlock_tryrlock(uint32_t id) {
    Lock lk(g);
    RwLock *r = get<RwLock>(id);
    if (!r) return ESRCH;
    if (r->writer) return EBUSY;
    r->readers++;
    return OK;
}
int32_t rwlock_runlock(uint32_t id) {
    Lock lk(g);
    RwLock *r = get<RwLock>(id);
    if (!r) return ESRCH;
    if (r->readers <= 0) return EPERM;
    if (--r->readers == 0) r->cv.notify_all();
    return OK;
}
int32_t rwlock_wlock(uint32_t id, uint64_t timeout) {
    Lock lk(g);
    RwLock *r = get<RwLock>(id);
    if (!r) return ESRCH;
    if (r->writer == me()) return EDEADLK;
    if (!wait(lk, r->cv, timeout, [&] { return r->writer == 0 && r->readers == 0; })) return ETIMEDOUT;
    r->writer = me();
    return OK;
}
int32_t rwlock_trywlock(uint32_t id) {
    Lock lk(g);
    RwLock *r = get<RwLock>(id);
    if (!r) return ESRCH;
    if (r->writer || r->readers) return EBUSY;
    r->writer = me();
    return OK;
}
int32_t rwlock_wunlock(uint32_t id) {
    Lock lk(g);
    RwLock *r = get<RwLock>(id);
    if (!r) return ESRCH;
    if (r->writer != me()) return EPERM;
    r->writer = 0;
    r->cv.notify_all();
    return OK;
}
} // namespace kern

namespace kern {
uint32_t equeue_by_key(uint64_t key) {
    Lock lk(g);
    for (auto &[id, o] : g_objs)
        if (auto *q = dynamic_cast<EQueue *>(o.get()); q && !q->dead && q->key == key) return id;
    return 0;
}
} // namespace kern
