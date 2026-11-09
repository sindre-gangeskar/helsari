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
#include <vector>

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
    mem::LineGuard g(ef);
    __atomic_fetch_or(flag_word(ef), __builtin_bswap32((uint32_t)bits << 16), __ATOMIC_SEQ_CST);
}

// Debug (with HELSARI_HOOK_WATCH): the last output of every Edge Zlib request by
// destination, so a later reader of a buffer can tell whether it still holds
// what was written (edge_zlib_explain), and a copy whether its source still
// holds what the inflate produced.
struct Snap { uint32_t src = 0, size = 0; bool inflate = false; uint32_t caller = 0; uint64_t seq = 0; std::vector<uint8_t> data; };
std::mutex g_snap_lock;
std::map<uint32_t, Snap> g_snaps;
std::atomic<uint64_t> g_snap_seq{0};
const bool g_snap_on = getenv("HELSARI_HOOK_WATCH") != nullptr;

// First offset in [0, n) where guest memory at ea differs from want, or -1.
long first_diff(uint32_t ea, const uint8_t *want, uint32_t n) {
    const uint8_t *m = (const uint8_t *)mem::ptr(ea);
    for (uint32_t k = 0; k < n; k++) if (m[k] != want[k]) return k;
    return -1;
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
        // HELSARI_DUMP_INFLATE: save input/output of blocks whose output has a long zero run, to check
        // the decoder against a reference (zlib) offline.
        static const bool dump = getenv("HELSARI_DUMP_INFLATE") != nullptr;
        static std::atomic<int> dumps{0};
        if (dump && n > 0 && dumps < 40) {
            const uint8_t *o = (const uint8_t *)mem::ptr(dst);
            long run = 0, best = 0;
            for (long k = 0; k < n; k++) run = o[k] ? 0 : run + 1, best = std::max(best, run);
            if (best >= 8192) {
                int id = dumps++;
                char path[64];
                snprintf(path, sizeof path, "inflate_%02d.in", id);
                if (FILE *f = fopen(path, "wb")) { fwrite(mem::ptr(src), 1, csize, f); fclose(f); }
                snprintf(path, sizeof path, "inflate_%02d.out", id);
                if (FILE *f = fopen(path, "wb")) { fwrite(o, 1, n, f); fclose(f); }
                LOG("spurs", "edge zlib: dumped block %d (zero run %ld) src=0x%x dst=0x%x", id, best, src, dst);
            }
        }
    } else {
        if (g_snap_on) {
            std::lock_guard<std::mutex> lk(g_snap_lock);
            auto it = g_snaps.find(src);
            if (it != g_snaps.end() && it->second.inflate && it->second.size >= csize) {
                long d = first_diff(src, it->second.data.data(), csize);
                if (d >= 0)
                    LOG("spurs", "edge zlib: copy 0x%x -> 0x%x: source differs from inflate #%llu (r13 0x%x) at +0x%lx "
                        "(now %08x, inflated %08x)", src, dst, (unsigned long long)it->second.seq, it->second.caller, d,
                        ld32(src + (uint32_t)(d & ~3)), __builtin_bswap32(*(uint32_t *)&it->second.data[d & ~3]));
            }
        }
        memmove(mem::ptr(dst), mem::ptr(src), csize);
        n = csize;
    }
    // HELSARI_WATCH_ZLIB_COPY=<first word>:<offset>:<len>: when a copy writes a block that
    // starts with that word, watch [offset, offset+len) of its output for later writes.
    static const char *wzc = getenv("HELSARI_WATCH_ZLIB_COPY");
    if (wzc && !(outv & 1) && n > 0) {
        uint32_t word = 0, off = 0, len = 0;
        sscanf(wzc, "%x:%x:%x", &word, &off, &len);
        if (ld32(dst) == word) {
            LOG("spurs", "edge zlib: copy -> 0x%x starts with 0x%08x: watching 0x%x+0x%x", dst, word, dst + off, len);
            mem::watch_add(dst + off, len);
        }
    }
    if (g_snap_on && n > 0) {
        std::lock_guard<std::mutex> lk(g_snap_lock);
        if (g_snaps.size() > 256 && !g_snaps.count(dst)) g_snaps.erase(g_snaps.begin());
        Snap &s = g_snaps[dst];
        s.src = src, s.size = (uint32_t)n, s.inflate = outv & 1, s.seq = ++g_snap_seq;
        s.data.assign((const uint8_t *)mem::ptr(dst), (const uint8_t *)mem::ptr(dst) + n);
    }
    if (mem::hook_watched(dst, usize))
        LOG("hook", "edge zlib %s 0x%x (%u bytes) -> 0x%x (%u): %ld; out %08x %08x %08x %08x", outv & 1 ? "inflate" : "copy",
            src, csize, dst, usize, n, ld32(dst), ld32(dst + 4), ld32(dst + 8), ld32(dst + 12));
    static int logged;
    if (logged++ < 8) LOG("spurs", "edge zlib: 0x%x (%u bytes) -> 0x%x: %ld of %u bytes, flag 0x%x bits 0x%x", src, csize, dst, n, usize, ef, bits);
    if (uint32_t ctr = outv & ~3u) {
        mem::LineGuard g(ctr);
        auto *p = (uint32_t *)mem::ptr(ctr);
        uint32_t old = __atomic_load_n(p, __ATOMIC_SEQ_CST);
        while (!__atomic_compare_exchange_n(p, &old, __builtin_bswap32(__builtin_bswap32(old) - 1), false,
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        }
    }
    if (ef) flag_set(ef, bits);
}
} // namespace

// Debug: explain the contents of [ea, ea+size) against the Edge Zlib outputs written there.
void edge_zlib_explain(uint32_t ea, uint32_t size) {
    if (!g_snap_on) return;
    std::lock_guard<std::mutex> lk(g_snap_lock);
    for (auto &[dst, s] : g_snaps) {
        if (ea >= dst + s.size || dst >= ea + size) continue;
        uint32_t lo = std::max(ea, dst), hi = std::min(ea + size, dst + s.size);
        long d = first_diff(lo, s.data.data() + (lo - dst), hi - lo);
        uint32_t nz = 0;
        for (uint32_t k = lo - dst; k < hi - dst; k++) nz += s.data[k] != 0;
        LOG("spurs", "  0x%x..0x%x came from edge zlib %s #%llu (0x%x -> 0x%x, r13 0x%x, %u nonzero bytes then; latest #%llu): %s",
            lo, hi, s.inflate ? "inflate" : "copy", (unsigned long long)s.seq, s.src, dst, s.caller, nz,
            (unsigned long long)g_snap_seq.load(), d < 0 ? "unchanged since" : "CHANGED since");
        if (d >= 0) LOG("spurs", "    first change at 0x%lx", lo + d);
    }
}

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
            mem::LineGuard g(ef);
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
    mem::LineGuard g(ARG32(0));
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
