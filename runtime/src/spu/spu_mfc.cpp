// SPU channels and the MFC (DMA engine). Transfers complete immediately, so
// tag-status reads never block. Lock-line atomics (GETLLAR/PUTLLC) keep a copy
// of the reserved 128-byte line and commit under the per-line lock that the
// PPU's stwcx./stdcx. also take, so a multi-word commit is all-or-nothing.
#include "spu.h"

#include "runtime.h"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace spu {
namespace {

uint8_t *ea_ptr(uint32_t ea) { return (uint8_t *)mem::ptr(ea); }

// KZ2_SPU_WATCH="ea:len,ea:len" logs every SPU transfer overlapping those
// guest ranges (who reads or writes a buffer), at most 20000 lines.
struct WatchRange { uint32_t ea, len; };
const std::vector<WatchRange> &watch_ranges() {
    static std::vector<WatchRange> v = [] {
        std::vector<WatchRange> r;
        if (const char *p = getenv("KZ2_SPU_WATCH"))
            for (const char *s = p; *s;) {
                char *end;
                uint32_t a = strtoul(s, &end, 16), n = *end == ':' ? strtoul(end + 1, &end, 16) : 4;
                r.push_back({a, n});
                s = *end == ',' ? end + 1 : end;
            }
        return r;
    }();
    return v;
}
void watch(const SpuThread &t, const char *what, uint32_t cmd, uint32_t ea, uint32_t lsa, uint32_t size) {
    static std::atomic<int> logs{0};
    for (auto &w : watch_ranges())
        if (ea < w.ea + w.len && w.ea < ea + size && logs++ < 20000)
            LOG("spu", "watch: %s %s cmd 0x%x ea 0x%x size 0x%x lsa 0x%x pc 0x%x", t.name.c_str(), what, cmd, ea, size, lsa,
                t.pc);
}

// Decrementer ticks at the 79.8 MHz timebase.
uint64_t ticks_now() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() * 798 / 10000;
}

bool line_changed(const SpuThread &t) {
    return t.raddr && memcmp(ea_ptr(t.raddr), t.rdata, 128) != 0;
}

} // namespace

uint32_t SpuThread::pending_events() {
    if ((event_mask & EVENT_LR) && line_changed(*this)) event_stat |= EVENT_LR;
    if (dec_armed && (int32_t)(dec_value - (uint32_t)(ticks_now() - dec_start)) < 0) {
        event_stat |= EVENT_TM;
        dec_armed = false;
    }
    return event_stat & event_mask;
}

void SpuThread::mfc_command(uint32_t cmd) {
    uint32_t lsa = mfc_lsa & (LS_SIZE - 1), ea = mfc_eal, size = mfc_size;
    // KZ2_SPU_TRACE logs commands while instructions are traced; KZ2_SPU_TRACE_MFC logs
    // every command of the traced workload, with lock-line contents for atomic stores.
    static const int64_t trace_limit = getenv("KZ2_SPU_TRACE") ? atoll(getenv("KZ2_SPU_TRACE")) : 0;
    static const bool trace_all = getenv("KZ2_SPU_TRACE_MFC") != nullptr;
    bool log = (traced && traced < trace_limit) || (trace_all && traced < ((int64_t)1 << 40));
    if (log) {
        LOG("spu", "%s   mfc cmd 0x%x lsa 0x%x ea 0x%x size 0x%x tag %u", name.c_str(), cmd, lsa, ea, size, mfc_tag);
        if (cmd == 0xb4 || cmd == 0xb0) {
            std::string hex;
            for (int i = 0; i < 32; i += 4) { char b[10]; snprintf(b, sizeof b, "%08x ", ls32((lsa & ~127u) + i)); hex += b; }
            LOG("spu", "%s     line: %s", name.c_str(), hex.c_str());
        }
    }
    // A transfer touching the NULL page is a bug upstream (on hardware it is a DMA fault).
    bool ea_cmd = (cmd >= 0x20 && cmd <= 0x22) || (cmd >= 0x30 && cmd <= 0x32) || (cmd >= 0x40 && cmd <= 0x42) ||
                  cmd == 0xb0 || cmd == 0xb4 || cmd == 0xb8 || cmd == 0xd0;
    if (ea_cmd && !watch_ranges().empty())
        watch(*this, (cmd & 0xf0) == 0x40 || cmd == 0xd0 ? "get" : "put", cmd, ea, lsa, cmd >= 0xb0 ? 128 : size);
    if (ea_cmd && ea < 0x10000) {
        static std::atomic<int> logs{0};
        if (logs++ < 10)
            LOG("spu", "%s: mfc cmd 0x%x on NULL page: ea 0x%x lsa 0x%x size 0x%x at pc 0x%x (lr 0x%x)", name.c_str(),
                cmd, ea, lsa, size, pc, gpr[0].w[0]);
    }
    switch (cmd) {
    case 0x40: case 0x41: case 0x42:                // GET, GETB, GETF
        memcpy(ls + lsa, ea_ptr(ea), size);
        code_loaded(lsa, ea, size);                 // unmaps what it overwrote; maps a known image
        break;
    case 0x20: case 0x21: case 0x22:                // PUT, PUTB, PUTF
    case 0x30: case 0x31: case 0x32:                // PUTR variants
        memcpy(ea_ptr(ea), ls + lsa, size);
        break;
    case 0x44: case 0x45: case 0x46:                // GETL
    case 0x24: case 0x25: case 0x26:                // PUTL
        run_list(cmd, mfc_tag & 31, lsa, ea & (LS_SIZE - 8), 0, size / 8);
        break;
    case 0xd0: {                                    // GETLLAR
        raddr = ea & ~127u;
        memcpy(rdata, ea_ptr(raddr), 128);
        ls_written(lsa & ~127u, 128);
        memcpy(ls + (lsa & ~127u), rdata, 128);
        atomic_stat = 4;                            // MFC_GETLLAR_SUCCESS
        break;
    }
    case 0xb4: {                                    // PUTLLC
        uint32_t line = ea & ~127u;
        bool ok = raddr == line;
        if (ok) {
            uint64_t *mem64 = (uint64_t *)ea_ptr(line);
            const uint64_t *want = (const uint64_t *)rdata, *src = (const uint64_t *)(ls + (lsa & ~127u));
            line_lock(line);
            for (int i = 0; ok && i < 16; i++) ok = __atomic_load_n(&mem64[i], __ATOMIC_SEQ_CST) == want[i];
            for (int i = 0; ok && i < 16; i++)
                if (want[i] != src[i]) __atomic_store_n(&mem64[i], src[i], __ATOMIC_SEQ_CST);
            if (ok) line_commit(line);
            line_unlock(line);
        }
        raddr = 0;
        atomic_stat = ok ? 0 : 1;                   // 1 = MFC_PUTLLC_FAILURE
        break;
    }
    case 0xb0: case 0xb8: {                         // PUTLLUC, PUTQLLUC
        line_lock(ea);
        memcpy(ea_ptr(ea & ~127u), ls + (lsa & ~127u), 128);
        line_commit(ea);
        line_unlock(ea);
        if (cmd == 0xb0) atomic_stat = 2;           // MFC_PUTLLUC_SUCCESS
        break;
    }
    case 0xc0: case 0xc8: case 0xcc:                // BARRIER, EIEIO, SYNC
        break;
    default:
        LOG("spu", "%s: unhandled MFC command 0x%x (lsa 0x%x ea 0x%x size 0x%x)", name.c_str(), cmd, lsa, ea, size);
        break;
    }
}

// Runs list elements [first, count): u32 (stall flag | size), u32 effective
// address each. An element with the stall flag pauses the list after its
// transfer until MFC_WrListStallAck for the tag; the Sn event announces it.
void SpuThread::run_list(uint32_t cmd, uint32_t tag, uint32_t lsa, uint32_t list, uint32_t first, uint32_t count) {
    bool get = cmd & 0x40;
    for (uint32_t i = first; i < count; i++) {
        uint32_t ts = ls32(list + 8 * i), eal = ls32(list + 8 * i + 4), sz = ts & 0x7fff;
        if (!watch_ranges().empty()) watch(*this, get ? "list get" : "list put", cmd, eal, lsa, sz);
        if (get) ls_written(lsa, sz), memcpy(ls + lsa, ea_ptr(eal), sz);
        else memcpy(ea_ptr(eal), ls + lsa, sz);
        lsa = (lsa + sz + 15) & (LS_SIZE - 16);
        if (ts & 0x80000000u) {
            stalled[tag] = {true, cmd, lsa, list, i + 1, count};
            stall_stat |= 1u << tag;
            event_stat |= EVENT_SN;
            return;
        }
    }
    stalled[tag].active = false;
}

uint32_t SpuThread::read_channel(int ch) {
    switch (ch) {
    case SPU_RdEventStat:
        // Blocks until an enabled event is pending.
        if (uint32_t e = pending_events()) return e;
        if (!event_mask) return 0;
        {
            auto t0 = std::chrono::steady_clock::now();
            uint32_t e;
            while (!(e = pending_events()) && event_mask) std::this_thread::yield();
            g_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            return e;
        }
    case SPU_RdEventMask: return event_mask;
    case SPU_RdDec: return dec_value - (uint32_t)(ticks_now() - dec_start);
    case SPU_RdMachStat: return interrupts ? 1 : 0;   // bit 0: interrupts enabled
    case SPU_RdSRR0: return srr0;
    case MFC_RdTagStat: {                           // transfers complete at once, except paused lists
        uint32_t busy = 0;
        for (int t = 0; t < 32; t++) if (stalled[t].active) busy |= 1u << t;
        return tag_mask & ~busy;
    }
    case MFC_RdListStallStat: { uint32_t s = stall_stat; stall_stat = 0; return s; }
    case MFC_RdAtomicStat: { uint32_t s = atomic_stat; atomic_stat = 0; return s; }
    case SPU_RdInMbox:
        if (!in_mbox.empty()) { uint32_t v = in_mbox.front(); in_mbox.pop_front(); return v; }
        [[fallthrough]];
    default:
        if (on_read_channel) return on_read_channel(*this, ch);
        LOG("spu", "%s: read of unhandled channel %d", name.c_str(), ch);
        return 0;
    }
}

void SpuThread::write_channel(int ch, uint32_t v) {
    switch (ch) {
    case SPU_WrOutMbox: out_mbox = v; break;
    case 69: case 70: case 71: break;               // performance bookmark / monitor start, stop: no effect
    case SPU_WrEventMask: event_mask = v; break;
    case SPU_WrEventAck: event_stat &= ~v; break;
    case SPU_WrDec: dec_value = v, dec_start = ticks_now(), dec_armed = (int32_t)v >= 0; break;
    case SPU_WrSRR0: srr0 = v; break;
    case MFC_LSA: mfc_lsa = v; break;
    case MFC_EAH: mfc_eah = v; break;
    case MFC_EAL: mfc_eal = v; break;
    case MFC_Size: mfc_size = v; break;
    case MFC_TagID: mfc_tag = v; break;
    case MFC_Cmd: mfc_command(v); break;
    case MFC_WrTagMask: tag_mask = v; break;
    case MFC_WrTagUpdate:
        // Transfers finish immediately, so a requested status update is ready at once.
        tag_update = v;
        if (v) event_stat |= EVENT_TG;
        break;
    case MFC_WrListStallAck: {
        StalledList &l = stalled[v & 31];
        if (l.active) run_list(l.cmd, v & 31, l.lsa, l.list, l.next, l.count);
        break;
    }
    default:
        if (on_write_channel) on_write_channel(*this, ch, v);
        else LOG("spu", "%s: write 0x%x to unhandled channel %d", name.c_str(), v, ch);
        break;
    }
}

uint32_t SpuThread::channel_count(int ch) {
    switch (ch) {
    case SPU_RdEventStat: return pending_events() ? 1 : 0;
    case MFC_Cmd: return 16;                       // free queue slots
    case MFC_RdAtomicStat: return atomic_stat ? 1 : 0;
    case MFC_RdListStallStat: return stall_stat ? 1 : 0;
    case SPU_RdInMbox: return !in_mbox.empty() || on_read_channel ? 1 : 0;
    case SPU_RdSigNotify1: case SPU_RdSigNotify2:
        return on_read_channel ? 1 : 0;
    default: return 1;
    }
}

} // namespace spu
