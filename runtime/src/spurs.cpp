// SPURS on the host. No SPU code runs: known SPU task programs are
// identified by their embedded ELF and replaced with native C++.
//
//   Edge Zlib inflate task  -> inflate_raw() on the PPU side
//
// SPURS event flags and LF queues are host objects keyed by the guest
// address of their structure.
#include "runtime.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <string>

#define REG(n) hle_register(#n, hle_##n)

long inflate_raw(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen);

namespace {
enum class TaskKind { Unknown, EdgeZlibInflate };

struct EventFlag { uint16_t bits = 0; uint32_t clear_mode = 0; std::condition_variable cv; };
struct LFQueue { uint32_t taskset = 0, entry_size = 0; };

std::mutex g_lock;
std::map<uint32_t, EventFlag> g_flags;
std::map<uint32_t, LFQueue> g_queues;
std::map<uint32_t, TaskKind> g_taskset_kind;
uint32_t g_next_task = 1;

TaskKind identify_task(uint32_t elf) {
    // SPU ELFs embedded in the game; look for a telltale string.
    const char *p = (const char *)mem::ptr(elf);
    std::string blob(p, 0x10000);
    if (blob.find("edgezlib_inflate") != std::string::npos) return TaskKind::EdgeZlibInflate;
    return TaskKind::Unknown;
}

void flag_set(uint32_t ef, uint16_t bits) {
    std::lock_guard<std::mutex> lk(g_lock);
    EventFlag &f = g_flags[ef];
    f.bits |= bits;
    f.cv.notify_all();
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
    std::lock_guard<std::mutex> lk(g_lock);
    EventFlag &f = g_flags[ARG32(2)];
    f.bits = 0;
    f.clear_mode = ARG32(3);
    memset(mem::ptr(ARG32(2)), 0, 128);
    RET(0);
}
HLE(cellSpursEventFlagWait) {
    // (eventFlag, u16 *mask, mode: 0 = OR, 1 = AND)
    uint32_t ef = ARG32(0), mp = ARG32(1), mode = ARG32(2);
    uint16_t want = ld16(mp);
    std::unique_lock<std::mutex> lk(g_lock);
    EventFlag &f = g_flags[ef];
    f.cv.wait(lk, [&] { return mode ? (f.bits & want) == want : (f.bits & want) != 0; });
    uint16_t got = f.bits & want;
    if (f.clear_mode == 0) f.bits &= ~got;  // CELL_SPURS_EVENT_FLAG_CLEAR_AUTO
    st16(mp, got);
    RET(0);
}
HLE(cellSpursEventFlagClear) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_flags[ARG32(0)].bits &= ~(uint16_t)ARG32(1);
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

HLE(cellSpursCreateTask) {
    // (taskset, u32 *taskId, elf, context, size, lsPattern, argument)
    TaskKind kind = identify_task(ARG32(2));
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (kind != TaskKind::Unknown) g_taskset_kind[ARG32(0)] = kind;
    }
    LOG("spurs", "task in taskset 0x%x: SPU ELF 0x%x -> %s", ARG32(0), ARG32(2),
        kind == TaskKind::EdgeZlibInflate ? "Edge Zlib inflate (native)" : "unknown (not run)");
    if (ARG32(1)) st32(ARG32(1), g_next_task++);
    RET(0);
}

void register_spurs() {
    REG(_cellSpursEventFlagInitialize); REG(cellSpursEventFlagWait); REG(cellSpursEventFlagClear);
    REG(cellSpursEventFlagAttachLv2EventQueue); REG(cellSpursEventFlagDetachLv2EventQueue);
    REG(_cellSpursLFQueueInitialize); REG(cellSpursLFQueueAttachLv2EventQueue);
    REG(cellSpursLFQueueDetachLv2EventQueue); REG(_cellSpursLFQueuePushBody); REG(cellSpursCreateTask);
}
