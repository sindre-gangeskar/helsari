// SPURS pieces that run natively: known SPU task programs identified by
// their embedded ELF and replaced with C++ (other tasks run on the SPU core,
// spurs_tasks.cpp), plus event flags and LF queues.
//
//   Edge Zlib inflate task  -> inflate_raw() on the PPU side
//
// Event flags use the real CellSpursEventFlag layout in guest memory, since
// SPU tasks set them with atomics: +0x00 u16 events, +0x0F clear mode.
#include "runtime.h"

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#define REG(n) hle_register(#n, hle_##n)

long inflate_raw(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen);

namespace {
enum class TaskKind { Unknown, EdgeZlibInflate };

struct LFQueue { uint32_t taskset = 0, entry_size = 0; };

std::mutex g_lock;
std::map<uint32_t, LFQueue> g_queues;
std::map<uint32_t, TaskKind> g_taskset_kind;

TaskKind identify_task(uint32_t elf) {
    // SPU ELFs embedded in the game; look for a telltale string.
    const char *p = (const char *)mem::ptr(elf);
    std::string blob(p, 0x10000);
    if (blob.find("edgezlib_inflate") != std::string::npos) return TaskKind::EdgeZlibInflate;
    return TaskKind::Unknown;
}

// The u16 event bits are the high half of the big-endian word at +0.
uint32_t *flag_word(uint32_t ef) { return (uint32_t *)mem::ptr(ef); }
void flag_set(uint32_t ef, uint16_t bits) {
    __atomic_fetch_or(flag_word(ef), __builtin_bswap32((uint32_t)bits << 16), __ATOMIC_SEQ_CST);
}

// Edge Zlib inflate queue element (32 bytes):
//   +0 eaCompressed, +4 eaUncompressed, +8 compressedSize, +12 uncompressedSize,
//   +16 eaTaskCounter | processType (bit0: 1 = inflate, 0 = copy); the task
//       atomically decrements the big-endian counter when done,
//   +20 eaEventFlag, +24 u16 eventFlagBits
void edge_zlib(uint32_t e) {
    uint32_t src = ld32(e), dst = ld32(e + 4), csize = ld32(e + 8), usize = ld32(e + 12);
    uint32_t outv = ld32(e + 16), ef = ld32(e + 20);
    uint16_t bits = ld16(e + 24);
    long n;
    if (outv & 1) {
        n = inflate_raw((const uint8_t *)mem::ptr(src), csize, (uint8_t *)mem::ptr(dst), usize);
        if (n < 0) LOG("spurs", "edge zlib: corrupt stream src=0x%x csize=%u usize=%u", src, csize, usize);
    } else {
        memmove(mem::ptr(dst), mem::ptr(src), csize);
        n = csize;
    }
    static int logged;
    if (logged++ < 8) LOG("spurs", "edge zlib: 0x%x (%u bytes) -> 0x%x: %ld of %u bytes, flag 0x%x bits 0x%x", src, csize, dst, n, usize, ef, bits);
    if (uint32_t ctr = outv & ~3u) {
        auto *p = (uint32_t *)mem::ptr(ctr);
        uint32_t old = __atomic_load_n(p, __ATOMIC_SEQ_CST);
        while (!__atomic_compare_exchange_n(p, &old, __builtin_bswap32(__builtin_bswap32(old) - 1), false,
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        }
    }
    if (ef) flag_set(ef, bits);
}
} // namespace

HLE(_cellSpursEventFlagInitialize) {
    // (spurs, taskset, eventFlag, flagClearMode, flagDirection)
    uint32_t ef = ARG32(2);
    memset(mem::ptr(ef), 0, 128);
    *(uint8_t *)mem::ptr(ef + 0x0e) = (uint8_t)ARG32(4);
    *(uint8_t *)mem::ptr(ef + 0x0f) = (uint8_t)ARG32(3);
    RET(0);
}
HLE(cellSpursEventFlagWait) {
    // (eventFlag, u16 *mask, mode: 0 = OR, 1 = AND). Setters may be SPU tasks
    // writing guest memory directly, so poll the event bits.
    uint32_t ef = ARG32(0), mp = ARG32(1), mode = ARG32(2);
    uint16_t want = ld16(mp);
    bool auto_clear = *(uint8_t *)mem::ptr(ef + 0x0f) == 0;               // CELL_SPURS_EVENT_FLAG_CLEAR_AUTO
    for (;;) {
        uint32_t old = __atomic_load_n(flag_word(ef), __ATOMIC_SEQ_CST);
        uint16_t bits = (uint16_t)(__builtin_bswap32(old) >> 16), got = bits & want;
        if (mode ? got == want : got != 0) {
            uint32_t now = auto_clear ? __builtin_bswap32(__builtin_bswap32(old) & ~((uint32_t)got << 16)) : old;
            if (!__atomic_compare_exchange_n(flag_word(ef), &old, now, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                continue;
            st16(mp, got);
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    RET(0);
}
HLE(cellSpursEventFlagClear) {
    __atomic_fetch_and(flag_word(ARG32(0)), ~__builtin_bswap32((uint32_t)(uint16_t)ARG32(1) << 16), __ATOMIC_SEQ_CST);
    RET(0);
}
HLE(cellSpursEventFlagAttachLv2EventQueue) { RET(0); }
HLE(cellSpursEventFlagDetachLv2EventQueue) { RET(0); }

HLE(_cellSpursLFQueueInitialize) {
    // (taskset, queue, buffer, entrySize, depth, direction)
    std::lock_guard<std::mutex> lk(g_lock);
    g_queues[ARG32(1)] = {ARG32(0), ARG32(3)};
    RET(0);
}
HLE(cellSpursLFQueueAttachLv2EventQueue) { RET(0); }
HLE(cellSpursLFQueueDetachLv2EventQueue) { RET(0); }
HLE(_cellSpursLFQueuePushBody) {
    // (queue, buffer, isBlocking): the consumer task runs synchronously
    TaskKind kind = TaskKind::Unknown;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto q = g_queues.find(ARG32(0));
        if (q != g_queues.end()) kind = g_taskset_kind[q->second.taskset];
    }
    if (kind == TaskKind::EdgeZlibInflate) edge_zlib(ARG32(1));
    else {
        static int n;
        if (n++ < 5) LOG("spurs", "LFQueue push to queue 0x%x with no native consumer", ARG32(0));
    }
    RET(0);
}

// Called when a task is created: true if it is replaced natively (and must
// not run on an SPU), remembering the replacement for the taskset's queues.
bool spurs_is_native_task(uint32_t taskset, uint32_t elf) {
    TaskKind kind = identify_task(elf);
    if (kind == TaskKind::Unknown) return false;
    std::lock_guard<std::mutex> lk(g_lock);
    g_taskset_kind[taskset] = kind;
    LOG("spurs", "task in taskset 0x%x: SPU ELF 0x%x -> Edge Zlib inflate (native)", taskset, elf);
    return true;
}

void register_spurs() {
    REG(_cellSpursEventFlagInitialize); REG(cellSpursEventFlagWait); REG(cellSpursEventFlagClear);
    REG(cellSpursEventFlagAttachLv2EventQueue); REG(cellSpursEventFlagDetachLv2EventQueue);
    REG(_cellSpursLFQueueInitialize); REG(cellSpursLFQueueAttachLv2EventQueue);
    REG(cellSpursLFQueueDetachLv2EventQueue); REG(_cellSpursLFQueuePushBody);
}
