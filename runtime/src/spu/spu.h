// SPU core: register file, local store, and the MFC channel interface.
// Execution is an interpreter (spu_interp.cpp); channels and DMA live in
// spu_mfc.cpp. Hosts (e.g. the SPURS kernel in spurs_kernel.cpp) drive a
// SpuThread and may trap LS addresses to run native code instead.
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

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
    int64_t traced = 0;                      // instructions logged so far (KZ2_SPU_TRACE)

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
    void set_ls32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(ls + (a & (LS_SIZE - 4)), &v, 4); }
    V lq(uint32_t a) const {
        a &= LS_SIZE - 16;
        V r;
        for (int i = 0; i < 4; i++) r.w[i] = ls32(a + 4 * i);
        return r;
    }
    void stq(uint32_t a, const V &v) {
        a &= LS_SIZE - 16;
        for (int i = 0; i < 4; i++) set_ls32(a + 4 * i, v.w[i]);
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

} // namespace spu
