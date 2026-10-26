// cellSync primitives. They live in guest memory and SPU code (libsync)
// operates on the same structures with atomics, so the layouts are real.
//
// CellSyncMutex (4 bytes, a ticket lock): +0 u16 rel, +2 u16 acq.
//   lock:    take ticket = acq++, wait until rel == ticket
//   trylock: only when rel == acq, then acq++
//   unlock:  rel++
#include "kernel.h"

#include <thread>

#define REG(n) hle_register(#n, hle_##n)

namespace {
constexpr int32_t SYNC_ERROR_BUSY = (int32_t)0x8041010a;
constexpr int32_t SYNC_ERROR_INVAL = (int32_t)0x80410102;
constexpr int32_t SYNC_ERROR_NULL_POINTER = (int32_t)0x80410111;

uint32_t *word(uint32_t ea) { return (uint32_t *)mem::ptr(ea); }
uint16_t rel_of(uint32_t w) { return (uint16_t)(__builtin_bswap32(w) >> 16); }
uint16_t acq_of(uint32_t w) { return (uint16_t)__builtin_bswap32(w); }
uint32_t make(uint16_t rel, uint16_t acq) { return __builtin_bswap32((uint32_t)rel << 16 | acq); }

int32_t check(uint32_t m) { return !m ? SYNC_ERROR_NULL_POINTER : (m & 3) ? SYNC_ERROR_INVAL : 0; }
} // namespace

HLE(cellSyncMutexInitialize) {
    uint32_t m = ARG32(0);
    if (int32_t e = check(m)) { RET(e); return; }
    { mem::LineGuard g(m); __atomic_store_n(word(m), 0u, __ATOMIC_SEQ_CST); }
    RET(0);
}
HLE(cellSyncMutexLock) {
    uint32_t m = ARG32(0);
    if (int32_t e = check(m)) { RET(e); return; }
    uint32_t old;
    {
        mem::LineGuard g(m);
        old = __atomic_load_n(word(m), __ATOMIC_SEQ_CST);
        __atomic_store_n(word(m), make(rel_of(old), acq_of(old) + 1), __ATOMIC_SEQ_CST);
    }
    uint16_t ticket = acq_of(old);
    while (rel_of(__atomic_load_n(word(m), __ATOMIC_SEQ_CST)) != ticket) std::this_thread::yield();
    RET(0);
}
HLE(cellSyncMutexTryLock) {
    uint32_t m = ARG32(0);
    if (int32_t e = check(m)) { RET(e); return; }
    {
        mem::LineGuard g(m);
        uint32_t old = __atomic_load_n(word(m), __ATOMIC_SEQ_CST);
        if (rel_of(old) != acq_of(old)) { RET(SYNC_ERROR_BUSY); return; }
        __atomic_store_n(word(m), make(rel_of(old), acq_of(old) + 1), __ATOMIC_SEQ_CST);
    }
    RET(0);
}
HLE(cellSyncMutexUnlock) {
    uint32_t m = ARG32(0);
    if (int32_t e = check(m)) { RET(e); return; }
    {
        mem::LineGuard g(m);
        uint32_t old = __atomic_load_n(word(m), __ATOMIC_SEQ_CST);
        __atomic_store_n(word(m), make(rel_of(old) + 1, acq_of(old)), __ATOMIC_SEQ_CST);
    }
    RET(0);
}

void register_sync() {
    REG(cellSyncMutexInitialize); REG(cellSyncMutexLock); REG(cellSyncMutexTryLock); REG(cellSyncMutexUnlock);
}
