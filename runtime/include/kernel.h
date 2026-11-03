// LV2 kernel objects (synchronisation, events). Shared by the syscall layer
// and the HLE libraries that sit on top of it (sysPrxForUser lwmutex etc.).
#pragma once
#include "runtime.h"

#include "no_errno_macros.h"

#include <cstdint>

namespace kern {
// LV2 error codes
constexpr int32_t OK = 0;
constexpr int32_t EAGAIN = (int32_t)0x80010001;
constexpr int32_t EINVAL = (int32_t)0x80010002;
constexpr int32_t ENOMEM = (int32_t)0x80010004;
constexpr int32_t ESRCH = (int32_t)0x80010005;
constexpr int32_t ENOENT = (int32_t)0x80010006;
constexpr int32_t EDEADLK = (int32_t)0x80010008;
constexpr int32_t EPERM = (int32_t)0x80010009;
constexpr int32_t EBUSY = (int32_t)0x8001000A;
constexpr int32_t ETIMEDOUT = (int32_t)0x8001000B;
constexpr int32_t ECANCELED = (int32_t)0x80010013;
constexpr int32_t EEXIST = (int32_t)0x80010014;
constexpr int32_t EISCONN = (int32_t)0x80010015;
constexpr int32_t ENOTCONN = (int32_t)0x80010016;
constexpr int32_t EBADF = (int32_t)0x8001002A;

// timeout is in microseconds, 0 = infinite
int32_t mutex_create(uint32_t *id, bool recursive);
int32_t mutex_destroy(uint32_t id);
int32_t mutex_lock(uint32_t id, uint64_t timeout);
int32_t mutex_trylock(uint32_t id);
int32_t mutex_unlock(uint32_t id);

int32_t cond_create(uint32_t *id, uint32_t mutex_id);
int32_t cond_destroy(uint32_t id);
int32_t cond_wait(uint32_t id, uint64_t timeout);
int32_t cond_signal(uint32_t id, bool all);

int32_t sema_create(uint32_t *id, int32_t initial, int32_t max);
int32_t sema_destroy(uint32_t id);
int32_t sema_wait(uint32_t id, uint64_t timeout);
int32_t sema_trywait(uint32_t id);
int32_t sema_post(uint32_t id, int32_t count);
int32_t sema_get_value(uint32_t id, int32_t *out);

int32_t eflag_create(uint32_t *id, uint64_t init);
int32_t eflag_destroy(uint32_t id);
int32_t eflag_wait(uint32_t id, uint64_t bits, uint32_t mode, uint64_t *result, uint64_t timeout);
int32_t eflag_trywait(uint32_t id, uint64_t bits, uint32_t mode, uint64_t *result);
int32_t eflag_set(uint32_t id, uint64_t bits);
int32_t eflag_clear(uint32_t id, uint64_t bits);
int32_t eflag_get(uint32_t id, uint64_t *out);

struct Event { uint64_t source, d1, d2, d3; };
int32_t equeue_create(uint32_t *id, uint64_t key, int size);
int32_t equeue_destroy(uint32_t id);
int32_t equeue_receive(uint32_t id, Event *ev, uint64_t timeout);
int32_t equeue_tryreceive(uint32_t id, Event *evs, int max, int *count);
int32_t equeue_drain(uint32_t id);
int32_t equeue_push(uint32_t id, const Event &ev);
uint32_t equeue_by_key(uint64_t key);  // 0 if none
int32_t eport_create(uint32_t *id, int type, uint64_t name);
int32_t eport_destroy(uint32_t id);
int32_t eport_connect(uint32_t port, uint32_t queue);
int32_t eport_disconnect(uint32_t port);
int32_t eport_send(uint32_t port, uint64_t d1, uint64_t d2, uint64_t d3);

int32_t rwlock_create(uint32_t *id);
int32_t rwlock_destroy(uint32_t id);
int32_t rwlock_rlock(uint32_t id, uint64_t timeout);
int32_t rwlock_tryrlock(uint32_t id);
int32_t rwlock_runlock(uint32_t id);
int32_t rwlock_wlock(uint32_t id, uint64_t timeout);
int32_t rwlock_trywlock(uint32_t id);
int32_t rwlock_wunlock(uint32_t id);
} // namespace kern
