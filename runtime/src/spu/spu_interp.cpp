// SPU interpreter: decodes and executes the full SPU instruction set.
// Registers are V (big-endian slot order, w[0] = preferred slot); see spu.h.
#include "spu_ops.h"

#include "runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace spu {
namespace {


// Decode table indexed by the top 11 bits of the instruction.
struct Decoder {
    uint8_t t[2048] = {};
    Decoder() {
        // Narrower opcodes first so exact 11-bit entries win.
        for (int bits : {4, 7, 8, 9, 10, 11})
            for (int op = 1; op < OP_COUNT; op++)
                if (k_ops[op].bits == bits) {
                    int span = 1 << (11 - bits);
                    for (int i = 0; i < span; i++) t[(k_ops[op].code << (11 - bits)) | i] = (uint8_t)op;
                }
    }
};
const Decoder g_dec;


} // namespace

std::string disasm(uint32_t pc, uint32_t x) {
    Op op = decode(x);
    const OpInfo &o = k_ops[op];
    uint32_t rt = x & 0x7f, ra = (x >> 7) & 0x7f, rb = (x >> 14) & 0x7f;
    char buf[96];
    switch (o.fmt) {
    case 'R': snprintf(buf, sizeof buf, "%s r%u,r%u,r%u", o.name, rt, ra, rb); break;
    case 'I': snprintf(buf, sizeof buf, "%s r%u,r%u,%d", o.name, rt, ra, sext(rb, 7)); break;
    case 'T': snprintf(buf, sizeof buf, "%s r%u,r%u,%d", o.name, rt, ra, sext((x >> 14) & 0x3ff, 10)); break;
    case '8': snprintf(buf, sizeof buf, "%s r%u,r%u,%u", o.name, rt, ra, (x >> 14) & 0xff); break;
    case 'S':   // absolute LS forms take a word address; constants show the immediate
        if (op == LQA || op == STQA || op == BRA || op == BRASL)
            snprintf(buf, sizeof buf, "%s r%u,0x%x", o.name, rt, ((x >> 7) & 0xffff) << 2 & (LS_SIZE - 4));
        else
            snprintf(buf, sizeof buf, "%s r%u,0x%x", o.name, rt, (x >> 7) & 0xffff);
        break;
    case 'B': snprintf(buf, sizeof buf, "%s r%u,0x%x", o.name, rt, (pc + (sext((x >> 7) & 0xffff, 16) << 2)) & (LS_SIZE - 4)); break;
    case 'L': snprintf(buf, sizeof buf, "%s r%u,0x%x", o.name, rt, (x >> 7) & 0x3ffff); break;
    case 'Q': snprintf(buf, sizeof buf, "%s r%u,r%u,r%u,r%u", o.name, (x >> 21) & 0x7f, ra, rb, rt); break;
    case 'C': snprintf(buf, sizeof buf, "%s r%u,ch%u", o.name, rt, ra); break;
    default: snprintf(buf, sizeof buf, "%s", o.name); break;
    }
    return buf;
}

void SpuThread::reset() {
    memset(gpr, 0, sizeof gpr);
    pc = 0, srr0 = 0, atomic_stat = 0, raddr = 0, event_mask = 0, event_stat = 0;
    tag_mask = 0, tag_update = 0;
}

void SpuThread::dump_history() {
    LOG("spu", "%s: last instructions before 0x%x:", name.c_str(), pc);
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t a = history[(history_pos + i) & 63];
        LOG("spu", "  %05x: %08x  %s", a, ls32(a), disasm(a, ls32(a)).c_str());
    }
    std::string regs;
    for (int r = 0; r < 8; r++) { char b[24]; snprintf(b, sizeof b, " r%d=%x", r, gpr[r].w[0]); regs += b; }
    LOG("spu", "%s:%s", name.c_str(), regs.c_str());
}

std::atomic<uint64_t> g_insns{0}, g_run_ns{0}, g_wait_ns{0};

// KZ2_SPU_HOTSPOTS=1: every 1024th instruction is sampled as (thread, pc, hash of
// the 256 code bytes around it - job managers load different code at the same
// LS address); stats_line dumps the top samples every 6th call.
namespace {
struct HotSample { uint64_t count = 0; std::string thread; uint32_t pc = 0; uint32_t code[6] = {}; };
std::mutex g_hot_lock;
std::unordered_map<uint64_t, HotSample> g_hot;
const bool g_hot_on = getenv("KZ2_SPU_HOTSPOTS") != nullptr;

void hot_sample(const SpuThread &t) {
    uint32_t base = t.pc & (LS_SIZE - 256), h = 2166136261u;
    for (uint32_t k = 0; k < 256; k += 4) h = (h ^ t.ls32(base + k)) * 16777619u;
    uint64_t key = ((uint64_t)h << 32) | t.pc;
    std::lock_guard<std::mutex> lk(g_hot_lock);
    HotSample &s = g_hot[key];
    if (!s.count++) {
        s.thread = t.name, s.pc = t.pc;
        for (int k = 0; k < 6; k++) s.code[k] = t.ls32(t.pc + 4 * k);
    }
}

void hot_dump() {
    std::vector<std::pair<uint64_t, HotSample>> v;
    uint64_t total = 0;
    {
        std::lock_guard<std::mutex> lk(g_hot_lock);
        for (auto &[k, s] : g_hot) { v.push_back({k, s}); total += s.count; }
        g_hot.clear();
    }
    if (!total) return;
    // "task 3 (ELF 0xc59980)" -> "task (ELF 0xc59980)": one program, whichever task slot runs it
    auto program = [](std::string n) {
        if (n.rfind("task ", 0) == 0) { size_t p = n.find(' ', 5); if (p != std::string::npos) n = "task" + n.substr(p); }
        return n;
    };
    std::map<std::string, uint64_t> by_prog;
    struct Block { uint64_t count = 0; std::string prog; uint32_t base = 0, hash = 0, top_pc = 0; uint64_t top = 0; uint32_t code[3]; };
    std::map<std::pair<std::string, uint64_t>, Block> by_block;
    for (auto &[k, s] : v) {
        std::string p = program(s.thread);
        by_prog[p] += s.count;
        Block &b = by_block[{p, (k >> 32) << 32 | (s.pc & (LS_SIZE - 256))}];
        b.count += s.count, b.prog = p, b.base = s.pc & (LS_SIZE - 256), b.hash = (uint32_t)(k >> 32);
        if (s.count > b.top) b.top = s.count, b.top_pc = s.pc, memcpy(b.code, s.code, sizeof b.code);
    }
    std::vector<std::pair<std::string, uint64_t>> pv(by_prog.begin(), by_prog.end());
    std::sort(pv.begin(), pv.end(), [](auto &a, auto &b) { return a.second > b.second; });
    LOG("spu", "SPU time by program (%llu samples, 1 per 1024 insns):", (unsigned long long)total);
    for (size_t i = 0; i < pv.size() && i < 20; i++) LOG("spu", "  %5.1f%%  %s", 100.0 * pv[i].second / total, pv[i].first.c_str());
    std::vector<Block> bv;
    for (auto &[k, b] : by_block) bv.push_back(b);
    std::sort(bv.begin(), bv.end(), [](auto &a, auto &b) { return a.count > b.count; });
    LOG("spu", "hottest 256-byte code blocks:");
    for (size_t i = 0; i < bv.size() && i < 25; i++) {
        auto &b = bv[i];
        LOG("spu", "  %5.1f%%  %-26s %05x (code %08x) top pc %05x: %s | %s | %s", 100.0 * b.count / total, b.prog.c_str(),
            b.base, b.hash, b.top_pc, disasm(b.top_pc, b.code[0]).c_str(), disasm(b.top_pc + 4, b.code[1]).c_str(),
            disasm(b.top_pc + 8, b.code[2]).c_str());
    }
}
} // namespace

std::string stats_line(double secs, uint32_t frames) {
    static int calls;
    if (g_hot_on && ++calls % 6 == 0) hot_dump();
    static uint64_t last_i, last_r, last_w;
    uint64_t i = g_insns.load(), r = g_run_ns.load(), w = g_wait_ns.load();
    uint64_t di = i - last_i, dr = r - last_r, dw = w - last_w;
    last_i = i, last_r = r, last_w = w;
    char b[200];
    // busy = time in run() not blocked on events, summed over SPU threads (in cores)
    double busy = (double)(dr - std::min(dr, dw)) / 1e9;
    snprintf(b, sizeof b, "SPU %.1fM insns/frame, %.0fM insns/s, %.2f cores busy (%.0fM insns/s per busy core)",
             frames ? di / 1e6 / frames : 0.0, di / 1e6 / secs, busy / secs, busy > 0 ? di / 1e6 / busy : 0.0);
    return b;
}

void SpuThread::run() {
    running = true;
    // KZ2_SPU_TRACE=N logs the first N instructions executed by each SPU thread.
    static const int64_t trace_limit = getenv("KZ2_SPU_TRACE") ? atoll(getenv("KZ2_SPU_TRACE")) : 0;
    uint32_t event_poll = 0;
    uint64_t executed = 0;
    auto started = std::chrono::steady_clock::now();
    struct Flush {   // account on every exit path
        uint64_t &n;
        std::chrono::steady_clock::time_point t0;
        ~Flush() {
            g_insns += n;
            g_run_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        }
    } flush{executed, started};
    while (running) {
        if (!(++executed & 1023) && g_hot_on) hot_sample(*this);
        // External interrupt: an enabled event is pending while interrupts are on. Checked
        // every 64 instructions: the check (lock-line compare, clock read) costs far more than
        // an instruction, and hardware delivers events asynchronously anyway.
        if (interrupts && !(++event_poll & 63) && pending_events()) {
            srr0 = pc;
            pc = 0;
            interrupts = false;
        }
        if (traps[pc >> 2] && on_trap) {
            if (!on_trap(*this)) { running = false; break; }
            continue;
        }
        uint32_t x = ls32(pc);
        history[history_pos++ & 63] = pc;
        if (traced < trace_limit && traced++ >= 0) LOG("spu", "%s %05x: %08x  %s", name.c_str(), pc, x, disasm(pc, x).c_str());
        switch (decode(x)) {
#define SPU_OP_STEP(e, name, bits, code, fmt) case e: pc = step<e>(*this, x, pc); break;
            SPU_OPS(SPU_OP_STEP)
#undef SPU_OP_STEP
        default: pc = step<INVALID>(*this, x, pc); break;
        }
    }
}

} // namespace spu
