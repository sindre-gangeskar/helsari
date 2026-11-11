// SPU core: register file, local store, and the MFC channel interface.
// Execution is an interpreter (spu_interp.cpp); channels and DMA live in
// spu_mfc.cpp. Hosts (e.g. the SPURS kernel in spurs_kernel.cpp) drive a
// SpuThread and may trap LS addresses to run native code instead.
#pragma once
#include "idle_backoff.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace spu {

constexpr uint32_t LS_SIZE = 0x40000;

// One 128-bit register. w[0] is the preferred slot (big-endian word 0).
struct V {
    uint32_t w[4];
    uint8_t b(int i) const { return (uint8_t)(w[i >> 2] >> (24 - 8 * (i & 3))); }
    void set_b(int i, uint8_t v) {
        int s = 24 - 8 * (i & 3);
        w[i >> 2] = (w[i >> 2] & ~(0xffu << s)) | ((uint32_t)v << s);
    }
    uint16_t h(int i) const { return (uint16_t)(w[i >> 1] >> ((i & 1) ? 0 : 16)); }
    void set_h(int i, uint16_t v) {
        int s = (i & 1) ? 0 : 16;
        w[i >> 1] = (w[i >> 1] & ~(0xffffu << s)) | ((uint32_t)v << s);
    }
    uint64_t d(int i) const { return ((uint64_t)w[2 * i] << 32) | w[2 * i + 1]; }
    void set_d(int i, uint64_t v) { w[2 * i] = (uint32_t)(v >> 32), w[2 * i + 1] = (uint32_t)v; }
    float f(int i) const { float r; memcpy(&r, &w[i], 4); return r; }
    void set_f(int i, float v) { memcpy(&w[i], &v, 4); }
    double df(int i) const { uint64_t u = d(i); double r; memcpy(&r, &u, 8); return r; }
    void set_df(int i, double v) { uint64_t u; memcpy(&u, &v, 8); set_d(i, u); }
};

// MFC channel numbers (CBE architecture).
enum Channel {
    SPU_RdEventStat = 0, SPU_WrEventMask = 1, SPU_WrEventAck = 2, SPU_RdSigNotify1 = 3,
    SPU_RdSigNotify2 = 4, SPU_WrDec = 7, SPU_RdDec = 8, SPU_RdEventMask = 11, SPU_RdMachStat = 13,
    SPU_WrSRR0 = 14, SPU_RdSRR0 = 15, MFC_LSA = 16, MFC_EAH = 17, MFC_EAL = 18, MFC_Size = 19,
    MFC_TagID = 20, MFC_Cmd = 21, MFC_WrTagMask = 22, MFC_WrTagUpdate = 23, MFC_RdTagStat = 24,
    MFC_RdListStallStat = 25, MFC_WrListStallAck = 26, MFC_RdAtomicStat = 27, SPU_WrOutMbox = 28,
    SPU_RdInMbox = 29, SPU_WrOutIntrMbox = 30,
};

// SPU events (SPU_RdEventStat bits).
constexpr uint32_t EVENT_LR = 0x400;   // lock-line reservation lost
constexpr uint32_t EVENT_TM = 0x020;   // decrementer passed zero
constexpr uint32_t EVENT_TG = 0x001;   // tag-group status update ready
constexpr uint32_t EVENT_SN = 0x002;   // DMA list stall-and-notify

// Recompiled SPU code (tools/recomp/spu.py, spu_native.cpp). A native function
// enters at one of its block starts and returns the next pc to run.
struct SpuThread;
using NativeFn = uint32_t (*)(SpuThread &, uint32_t pc);
struct NativeEntry { uint32_t pc; NativeFn fn; };
// One compiled image, executed at LS ls_base: `size` bytes found at guest address
// src_ea (EBOOT modules), or captured at runtime and embedded as `image`
// (spu_cache modules, see spu_capture.cpp). protect marks the 128-byte lines
// holding code (null: all); a runtime module only compiles and matches those.
struct NativeModule {
    const char *name;
    uint32_t src_ea, ls_base, size;
    const NativeEntry *entries;
    uint32_t count;
    const uint8_t *image;
    const uint8_t *protect;
};

struct SpuThread {
    alignas(16) uint8_t ls[LS_SIZE];
    V gpr[128];
    uint32_t pc = 0;
    uint32_t srr0 = 0;
    bool running = false;
    std::string name;
    uint32_t history[64] = {};             // recently executed PCs, for crash dumps
    uint32_t history_pos = 0;
    void dump_history();
    int64_t traced = 0;                      // instructions logged so far (HELSARI_SPU_TRACE)

    // MFC command parameters (channels 16-21)
    uint32_t mfc_lsa = 0, mfc_eah = 0, mfc_eal = 0, mfc_size = 0, mfc_tag = 0;
    uint32_t tag_mask = 0, tag_update = 0;
    uint32_t atomic_stat = 0;
    alignas(16) uint8_t rdata[128];        // GETLLAR reservation copy
    uint32_t raddr = 0;                    // reserved line (0 = none)
    uint32_t event_mask = 0, event_stat = 0;
    uint64_t dec_start = 0;                // host time when decrementer was written
    uint32_t dec_value = 0;
    bool dec_armed = false;                // decrementer event pending its zero crossing
    bool interrupts = false;               // enabled by the E bit of bi/iret, cleared by D
    // DMA lists paused at a stall-and-notify element, per tag.
    struct StalledList { bool active; uint32_t cmd, lsa, list, next, count; };
    StalledList stalled[32] = {};
    uint32_t stall_stat = 0;
    // Mailboxes: the outbound value is read by the host when the SPU writes the
    // outbound interrupt mailbox (sys_spu_thread_send_event); the host answers
    // through the inbound queue.
    uint32_t out_mbox = 0;
    std::deque<uint32_t> in_mbox;

    // Host hooks. stop(code) runs on STOP/STOPD and returns false to end
    // execution. trap(pc) runs when execution reaches an address marked with
    // set_trap(); it may change pc/registers and returns false to end.
    std::function<bool(SpuThread &, uint32_t code)> on_stop;
    std::function<bool(SpuThread &)> on_trap;
    std::function<uint32_t(SpuThread &, int ch)> on_read_channel;          // unhandled channels
    std::function<void(SpuThread &, int ch, uint32_t v)> on_write_channel;
    uint8_t traps[LS_SIZE / 4] = {};
    void set_trap(uint32_t ls_addr) { traps[(ls_addr & (LS_SIZE - 1)) >> 2] = 1; }

    // LS access (big-endian, wraps at 256 KB)
    uint32_t ls32(uint32_t a) const {
        uint32_t v; memcpy(&v, ls + (a & (LS_SIZE - 4)), 4); return __builtin_bswap32(v);
    }
    void set_ls32(uint32_t a, uint32_t v) {
        a &= LS_SIZE - 4;
        if (__builtin_expect(code_q[a >> 4] == CODE, 0)) ls_written_cold(a, 4);
        v = __builtin_bswap32(v);
        memcpy(ls + a, &v, 4);
    }
    // A quadword in LS is 16 big-endian bytes; V keeps big-endian word order with each
    // word in host order, so converting is one byte swap per word.
    static __m128i bswap32_mask() { return _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12); }
    V lq(uint32_t a) const {
        V r;
        _mm_storeu_si128((__m128i *)r.w,
                         _mm_shuffle_epi8(_mm_load_si128((const __m128i *)(ls + (a & (LS_SIZE - 16)))), bswap32_mask()));
        return r;
    }
    void stq(uint32_t a, const V &v) {
        a &= LS_SIZE - 16;
        if (__builtin_expect(code_q[a >> 4] == CODE, 0)) ls_written_cold(a, 16);
        _mm_store_si128((__m128i *)(ls + a), _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)v.w), bswap32_mask()));
    }

    // Recompiled code mapped into LS. native[pc / 4] is the function entering
    // at pc; code_q marks the quadwords of mapped modules: a write to a CODE
    // quadword unmaps its module (code_epoch++ makes running native code
    // return); DATA (the rest of the image) may change freely - nothing was
    // compiled from it. Quadwords, not lines: code and data share 128-byte lines.
    enum : uint8_t { CODE = 1, DATA = 2 };
    std::unique_ptr<NativeFn[]> native;   // owned: a SpuThread per SPURS task start (~200/s) leaked 512 KB each
    uint8_t code_q[LS_SIZE / 16] = {};
    uint32_t code_epoch = 0;
    bool interp_once = false;              // native code handed one instruction to the interpreter
    uint32_t native_polls = 0;
    struct Mapped { const NativeModule *m; };
    Mapped mapped[8] = {};
    // An image was copied to LS [lsa, lsa+size) from guest address ea: map recompiled code for it.
    void code_loaded(uint32_t lsa, uint32_t ea, uint32_t size);
    bool map_module(const NativeModule *m);
    bool code_dirty = true;                // LS written since map_at last looked
    // Per 128-byte line: written since map_at last looked at a pc in it. map_at runs at the
    // first interpreted pc of each such line - not just at the first interpreted pc after a
    // write, which is usually the job kernel, leaving the job's own code (jumped to later)
    // interpreted for good.
    uint8_t line_seen[LS_SIZE / 128] = {};   // 0: fresh
    void mark_fresh(uint32_t lsa, uint32_t size) {
        if (!size) return;
        for (uint32_t l = lsa >> 7, e = ((lsa + size - 1) & (LS_SIZE - 1)) >> 7; l <= e; l++) line_seen[l] = 0;
    }
    bool map_at(uint32_t pc);              // map a module whose image LS holds at pc
    // Modules mapped most recently, tried first by map_at: job managers reload the
    // same few job binaries over and over, so the match is nearly always here.
    const NativeModule *recent[8] = {};
    // A DMA bringing bytes LS already holds over mapped code (job managers re-send the
    // same job binary before every run): nothing changes, so the mapping stays.
    // Saves an unmap, an image match and a remap per job. HELSARI_SPU_RELOAD_CHECK=0: off.
    bool same_code_reload(uint32_t lsa, const void *src, uint32_t size, uint32_t ea = 0) {
        static const bool on = [] { const char *e = getenv("HELSARI_SPU_RELOAD_CHECK"); return !e || atoi(e) != 0; }();
        if (!on || !size || lsa + size > LS_SIZE) return false;
        // An EBOOT job still mapped where it is being loaded again: the source is the game's
        // read-only image, and any write to that LS range would have unmapped it. Same bytes,
        // nothing to compare. (Only whole-code modules: others have data that may have changed.)
        if (ea)
            for (auto &mp : mapped)
                if (mp.m && !mp.m->protect && !mp.m->image && ea >= mp.m->src_ea && ea - mp.m->src_ea == lsa - mp.m->ls_base &&
                    lsa >= mp.m->ls_base && lsa + size <= mp.m->ls_base + mp.m->size)   // whole, or one piece of it
                    return true;
        bool code = false;
        for (uint32_t q = lsa >> 4, e = (lsa + size - 1) >> 4; q <= e && !code; q++) code = code_q[q] == CODE;
        return code && !memcmp(ls + lsa, src, size);
    }
    // LS [lsa, lsa+size) is about to be written by anything other than a code load.
    __attribute__((noinline, cold)) void ls_written_cold(uint32_t lsa, uint32_t size) { ls_written(lsa, size); }
    void ls_written(uint32_t lsa, uint32_t size) {
        code_dirty = true;
        mark_fresh(lsa, size);
        if (cap_any && size) capture_overwritten(lsa, size);
        for (uint32_t q = lsa >> 4, e = ((lsa + size - 1) & (LS_SIZE - 1)) >> 4; q <= e && size; q++)
            if (code_q[q] == CODE) { unmap_range(lsa, size); return; }
    }

    // Runtime capture of interpreted code (spu_capture.cpp): src_delta[line] is
    // ea - lsa of the DMA load that filled the line (0: not loaded), so lines
    // loaded together form one image; exec_q marks quadwords the interpreter ran.
    // Each image is saved to spu_cache/ for the recompiler when it is replaced.
    uint32_t src_delta[LS_SIZE / 128] = {};
    uint8_t exec_q[LS_SIZE / 16] = {};
    struct Capture { uint32_t lo, hi; bool valid; std::vector<uint8_t> image; uint64_t last_use; };
    Capture caps[4] = {};
    bool cap_any = false;
    uint64_t steps_total = 0, interp_total = 0;   // dispatches (compiled-code calls + interpreted) and interpreted, for statistics
    uint64_t waited_ns = 0;                // time blocked reading the event channel (task statistics)
    uint64_t wait_ns() const { return waited_ns; }
    IdleBackoff llar_idle;                 // GETLLAR re-reading an unchanged line at once (spu_mfc.cpp)
    uint64_t cap_clock = 0;
    void loaded_from(uint32_t lsa, uint32_t ea, uint32_t size) {
        mark_fresh(lsa, size);
        for (uint32_t l = lsa >> 7, e = ((lsa + size - 1) & (LS_SIZE - 1)) >> 7; l <= e && size; l++) src_delta[l] = ea - lsa;
    }
    // Interpreter hook: pc is about to run without native code.
    // The interpreter reached mapped module code that has no compiled entry (a computed
    // jump the recompiler could not see): saved for the next build (spu_capture.cpp).
    uint32_t last_interp = ~0u, cap_hot_lo = 0, cap_hot_size = 0;
    void record_entry(uint32_t pc);
    void note_interpreted(uint32_t pc) {
        exec_q[pc >> 4] |= 0x10 | (1 << ((pc >> 2) & 3));   // per word (see spu_capture.cpp)
        if (pc - cap_hot_lo < cap_hot_size) return;          // the slot of the last call: nearly always
        for (auto &c : caps)
            if (pc >= c.lo && pc < c.hi) {
                if (!(++cap_clock & 0x3fffff)) checkpoint_captures();
                c.last_use = cap_clock;
                cap_hot_lo = c.lo, cap_hot_size = c.hi - c.lo;
                return;
            }
        cap_hot_size = 0;
        begin_capture(pc);
    }
    void begin_capture(uint32_t pc);
    void capture_overwritten(uint32_t lsa, uint32_t size);
    void flush_capture(Capture &c);
    void checkpoint_captures();
    void unmap_range(uint32_t lsa, uint32_t size);
    // Called by native code at branches while interrupts are enabled: take a pending event.
    bool poll_interrupt(uint32_t &next) {
        if (++native_polls & 15 || !pending_events()) return false;
        srr0 = next, next = 0, interrupts = false;
        return true;
    }

    void reset();
    void run();                     // execute from pc until stopped
    uint32_t read_channel(int ch);
    void write_channel(int ch, uint32_t v);
    uint32_t channel_count(int ch);
    void mfc_command(uint32_t cmd);
    void run_list(uint32_t cmd, uint32_t tag, uint32_t lsa, uint32_t list, uint32_t first, uint32_t count);
    uint32_t pending_events();             // refresh event_stat from its sources; masked result
};

// Disassemble one instruction (for traces).
std::string disasm(uint32_t pc, uint32_t insn);

// Totals over all SPU threads, for the flip log: instructions executed, and
// nanoseconds spent in run() and blocked reading SPU_RdEventStat.
extern std::atomic<uint64_t> g_insns, g_run_ns, g_wait_ns;
std::string stats_line(double secs, uint32_t frames);   // since the previous call

} // namespace spu
