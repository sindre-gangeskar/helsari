// SPURS custom workloads: the game registers its own SPU policy modules
// (e.g. the Job Manager) with cellSpursAddWorkloadWithAttribute and wakes
// them with cellSpursReadyCountStore. Each workload gets a host thread with
// its own SpuThread that runs the policy module the way the SPURS kernel
// does: module at LS 0xA00, kernel context at LS 0x100, r3 = context,
// r4 = workload argument. The kernel's "select workload" and "exit" entry
// points are trapped and serviced natively.
#include "kernel.h"
#include "spu/spu.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#define REG(n) hle_register(#n, hle_##n)

void spurs_connect_events(spu::SpuThread &s, uint32_t spurs);

namespace {

constexpr uint32_t PM_ADDR = 0xa00;
constexpr uint32_t CTX_ADDR = 0x100;
constexpr uint32_t SELECT_WORKLOAD_ADDR = 0x290;   // kernel entry points the module calls
constexpr uint32_t EXIT_ADDR = 0x808;            // CELL_SPURS_KERNEL1_EXIT_ADDR

// CellSpursWorkloadAttribute is opaque to the game; this is our layout.
enum AttrOff { A_PM = 8, A_SIZE = 12, A_DATA = 16, A_MINC = 24, A_MAXC = 28, A_CLASS = 32, A_INSTANCE = 36, A_PRIO = 40 };

struct Workload {
    uint32_t id = 0, spurs = 0, pm = 0, size = 0;
    uint64_t data = 0;
    std::string name;
    // Ready count and contention live in the CellSpurs structure in guest
    // memory (wklReadyCount1 at +0x00, wklCurrentContention at +0x10, one byte
    // per workload id), where the SPU-side module updates them with atomics.
    uint8_t *ready_byte() const { return (uint8_t *)mem::ptr(spurs + 0x00 + id); }
    uint8_t *contention_byte() const { return (uint8_t *)mem::ptr(spurs + 0x10 + id); }
    uint8_t ready() const { return __atomic_load_n(ready_byte(), __ATOMIC_SEQ_CST); }
    std::atomic<bool> running{false}, shutdown{false};
    std::mutex m;
    std::condition_variable cv;
    std::unique_ptr<spu::SpuThread> spu;
};

std::mutex g_lock;
std::map<uint32_t, std::unique_ptr<Workload>> g_workloads;
uint32_t g_next_wid = 0;

Workload *find(uint32_t wid) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_workloads.find(wid);
    return it == g_workloads.end() ? nullptr : it->second.get();
}

void init_context(Workload &w, spu::SpuThread &s) {
    // SpursKernelContext fields the policy module reads.
    s.ls_written(CTX_ADDR, 0x100);
    memset(s.ls + CTX_ADDR, 0, 0x100);
    s.set_ls32(CTX_ADDR + 0xc0, 0), s.set_ls32(CTX_ADDR + 0xc4, w.spurs);    // 0x1C0: spurs (u64)
    s.set_ls32(CTX_ADDR + 0xc8, 0);                                           // 0x1C8: spuNum
    s.set_ls32(CTX_ADDR + 0xcc, 31);                                          // 0x1CC: dmaTagId
    s.set_ls32(CTX_ADDR + 0xd0, 0), s.set_ls32(CTX_ADDR + 0xd4, w.pm);       // 0x1D0: wklCurrentAddr
    s.set_ls32(CTX_ADDR + 0xd8, w.size);                                      // 0x1D8: wklCurrentUniqueId / size
    s.set_ls32(CTX_ADDR + 0xdc, w.id);                                        // 0x1DC: wklCurrentId
    s.set_ls32(CTX_ADDR + 0xe0, EXIT_ADDR);                                   // 0x1E0: exitToKernelAddr
    s.set_ls32(CTX_ADDR + 0xe4, SELECT_WORKLOAD_ADDR);                        // 0x1E4: selectWorkloadAddr
}

// Runs the policy module whenever the workload has work (ready count > 0).
void workload_thread(Workload *w) {
    auto &s = *w->spu;
    s.name = "spurs:" + (w->name.empty() ? std::to_string(w->id) : w->name);
    name_host_thread(s.name);
    if (const char *only = getenv("KZ2_SPU_TRACE_NAME"))
        if (s.name.find(only) == std::string::npos) s.traced = (int64_t)1 << 40;   // exclude from tracing
    s.ls_written(PM_ADDR, w->size);
    memcpy(s.ls + PM_ADDR, mem::ptr(w->pm), w->size);
    s.set_trap(SELECT_WORKLOAD_ADDR);
    s.set_trap(EXIT_ADDR);
    s.on_trap = [w](spu::SpuThread &t) {
        if (t.pc == EXIT_ADDR) return false;            // module returned to the kernel
        // Select workload / poll status: keep running this workload unless it
        // has been shut down. Result in r3: non-zero asks the module to exit.
        t.gpr[3] = spu::V{{w->shutdown ? 1u : 0u, 0, 0, 0}};
        t.pc = t.gpr[0].w[0] & (spu::LS_SIZE - 4);       // return to the caller (link in r0)
        return true;
    };
    s.on_stop = [](spu::SpuThread &t, uint32_t code) {
        LOG("spurs", "%s: stop 0x%x at 0x%x", t.name.c_str(), code, t.pc);
        t.dump_history();
        return false;
    };
    spurs_connect_events(s, w->spurs);
    LOG("spurs", "workload %u '%s': policy module 0x%x (%u bytes), arg 0x%llx", w->id, w->name.c_str(), w->pm,
        w->size, (unsigned long long)w->data);
    uint64_t runs = 0;
    while (!w->shutdown) {
        {
            std::unique_lock<std::mutex> lk(w->m);
            w->cv.wait_for(lk, std::chrono::milliseconds(1), [w] { return w->ready() > 0 || w->shutdown; });
        }
        if (w->shutdown) break;
        if (w->ready() == 0) continue;
        init_context(*w, s);
        for (auto &g : s.gpr) g = spu::V{};
        s.gpr[1] = spu::V{{0x3fff0, 0, 0, 0}};           // stack pointer at the top of LS
        s.gpr[3] = spu::V{{CTX_ADDR, 0, 0, 0}};
        s.gpr[4] = spu::V{{(uint32_t)(w->data >> 32), (uint32_t)w->data, 0, 0}};
        s.pc = PM_ADDR;
        s.interrupts = false, s.event_mask = 0, s.event_stat = 0, s.dec_armed = false, s.stall_stat = 0;
        for (auto &l : s.stalled) l.active = false;
        w->running = true;
        { mem::LineGuard g(w->spurs); __atomic_store_n(w->contention_byte(), 1, __ATOMIC_SEQ_CST); }
        // Log the first few runs of each workload, then every 1000th.
        bool log = runs < 5 || runs % 1000 == 0;
        if (log) LOG("spurs", "%s: run #%llu (ready %d)", s.name.c_str(), (unsigned long long)runs, w->ready());
        s.run();
        if (log) LOG("spurs", "%s: exited at 0x%x", s.name.c_str(), s.pc);
        runs++;
        { mem::LineGuard g(w->spurs); __atomic_store_n(w->contention_byte(), 0, __ATOMIC_SEQ_CST); }
        w->running = false;
    }
}

// SPU ports attached to event queues with cellSpursAttachLv2EventQueue:
// spurs -> port -> queue. Every SPU thread of that SPURS instance can send on them.
std::mutex g_port_lock;
std::map<uint32_t, std::map<uint32_t, uint32_t>> g_ports;

} // namespace

// Services the outbound interrupt mailbox of an SPU thread running for `spurs`.
// The top byte of the value selects the call (as sys_spu_thread_* encode it):
//   0-63    send_event(port, data0, data1): queued event, result to the inbound mailbox
//   64-127  throw_event: same event, no result
//   128/192 event_flag_set_bit(flag id in the outbound mailbox, bit); 128 waits for a result
void spurs_connect_events(spu::SpuThread &s, uint32_t spurs) {
    s.on_write_channel = [spurs](spu::SpuThread &c, int ch, uint32_t v) {
        if (ch != spu::SPU_WrOutIntrMbox) {
            LOG("spu", "%s: write 0x%x to unhandled channel %d", c.name.c_str(), v, ch);
            return;
        }
        uint32_t code = v >> 24;
        int32_t r;
        if (code < 128) {
            uint32_t port = code & 63, queue = 0;
            {
                std::lock_guard<std::mutex> lk(g_port_lock);
                auto &ports = g_ports[spurs];
                auto it = ports.find(port);
                if (it != ports.end()) queue = it->second;
            }
            // SYS_SPU_THREAD_EVENT_USER_KEY; d1 = SPU thread id, d2 = port << 32 | data0, d3 = data1
            r = queue ? kern::equeue_push(queue, {0xFFFFFFFF53505501ull, 0, ((uint64_t)port << 32) | (v & 0xffffff),
                                            c.out_mbox})
                      : (int32_t)0x80010016;                 // ENOTCONN
            static std::atomic<int> logs{0};
            if (r || logs++ < 100)
                LOG("spurs", "%s: %s_event port %u -> queue 0x%x (data 0x%x 0x%x): 0x%x", c.name.c_str(),
                    code < 64 ? "send" : "throw", port, queue, v & 0xffffff, c.out_mbox, r);
            if (code < 64) c.in_mbox.push_back(r);
        } else if (code == 128 || code == 192) {
            r = kern::eflag_set(c.out_mbox, 1ull << (v & 63));
            if (code == 128) c.in_mbox.push_back(r);
        } else {
            LOG("spurs", "%s: unknown outbound interrupt mailbox call 0x%x", c.name.c_str(), v);
            c.in_mbox.push_back(0x80010005);                         // ESRCH
        }
    };
}

HLE(cellSpursAttachLv2EventQueue) {
    // (spurs, queue, u8 *port, isDynamic): dynamic ports are picked from 0x10..0x3F.
    uint32_t spurs = ARG32(0), queue = ARG32(1), port_ptr = ARG32(2);
    bool dynamic = ARG32(3);
    if (!spurs || !port_ptr) RET(0x80410711);                               // CELL_SPURS_CORE_ERROR_NULL_POINTER
    std::lock_guard<std::mutex> lk(g_port_lock);
    auto &ports = g_ports[spurs];
    uint32_t port = dynamic ? 0x10 : ld8(port_ptr);
    if (dynamic) {
        while (port < 64 && ports.count(port)) port++;
        if (port == 64) RET(0x80410701);                                    // CELL_SPURS_CORE_ERROR_AGAIN
    } else if (port >= 64) {
        RET(0x80410702);                                                    // CELL_SPURS_CORE_ERROR_INVAL
    } else if (ports.count(port)) {
        RET(0x8041070a);                                                    // CELL_SPURS_CORE_ERROR_BUSY
    }
    ports[port] = queue;
    st8(port_ptr, port);
    LOG("spurs", "spurs 0x%x: event queue 0x%x attached to SPU port %u", spurs, queue, port);
    RET(0);
}
HLE(cellSpursDetachLv2EventQueue) {
    // (spurs, port)
    std::lock_guard<std::mutex> lk(g_port_lock);
    RET(g_ports[ARG32(0)].erase(ARG32(1) & 0xff) ? 0 : 0x80410705);       // CELL_SPURS_CORE_ERROR_SRCH
}

HLE(_cellSpursWorkloadAttributeInitialize) {
    // (attr, revision, sdkVersion, pm, size, u64 data, const u8 priority[8], minContention, maxContention)
    uint32_t a = ARG32(0);
    memset(mem::ptr(a), 0, 512);
    st32(a + A_PM, ARG32(3));
    st32(a + A_SIZE, ARG32(4));
    st64(a + A_DATA, ARG(5));
    if (ARG32(6)) memcpy(mem::ptr(a + A_PRIO), mem::ptr(ARG32(6)), 8);
    st32(a + A_MINC, ARG32(7));
    st32(a + A_MAXC, ld32(ctx->r[1] + 112));             // 9th argument is on the stack
    RET(0);
}
HLE(cellSpursWorkloadAttributeSetName) {
    st32(ARG32(0) + A_CLASS, ARG32(1));
    st32(ARG32(0) + A_INSTANCE, ARG32(2));
    RET(0);
}
HLE(cellSpursAddWorkloadWithAttribute) {
    // (spurs, u32 *wid, attr)
    uint32_t a = ARG32(2);
    auto w = std::make_unique<Workload>();
    w->spurs = ARG32(0);
    w->pm = ld32(a + A_PM), w->size = ld32(a + A_SIZE), w->data = ld64(a + A_DATA);
    if (ld32(a + A_INSTANCE)) w->name = mem::cstr(ld32(a + A_INSTANCE));
    w->spu = std::make_unique<spu::SpuThread>();
    Workload *p = w.get();
    {
        std::lock_guard<std::mutex> lk(g_lock);
        p->id = g_next_wid++;
        g_workloads[p->id] = std::move(w);
    }
    st32(ARG32(1), p->id);
    std::thread(workload_thread, p).detach();
    RET(0);
}
HLE(cellSpursReadyCountStore) {
    // (spurs, wid, value)
    Workload *w = find(ARG32(1));
    if (!w) { RET(0x80410711); return; }                 // CELL_SPURS_CORE_ERROR_INVAL
    static std::atomic<int> logged{0};
    if (logged++ < 30) LOG("spurs", "ready count of workload %u '%s' = %d", w->id, w->name.c_str(), (int32_t)ARG32(2));
    { mem::LineGuard g(w->spurs); __atomic_store_n(w->ready_byte(), (uint8_t)ARG32(2), __ATOMIC_SEQ_CST); }
    w->cv.notify_all();
    RET(0);
}
HLE(cellSpursGetWorkloadInfo) {
    // (spurs, wid, CellSpursWorkloadInfo *info)
    uint32_t info = ARG32(2);
    memset(mem::ptr(info), 0, 48);
    if (Workload *w = find(ARG32(1))) {
        st64(info + 0, w->data);
        st32(info + 16, w->pm);
        st32(info + 20, w->size);
        *(uint8_t *)mem::ptr(info + 32) = *w->contention_byte();   // contention
        *(uint8_t *)mem::ptr(info + 35) = w->ready();               // readyCount
    }
    RET(0);
}
HLE(cellSpursShutdownWorkload) {
    if (Workload *w = find(ARG32(1))) { w->shutdown = true; w->cv.notify_all(); }
    RET(0);
}
HLE(cellSpursWaitForWorkloadShutdown) {
    if (Workload *w = find(ARG32(1)))
        while (w->running) std::this_thread::yield();
    RET(0);
}
HLE(cellSpursRemoveWorkload) { RET(0); }

void register_spurs_kernel() {
    REG(_cellSpursWorkloadAttributeInitialize); REG(cellSpursWorkloadAttributeSetName);
    REG(cellSpursAddWorkloadWithAttribute); REG(cellSpursReadyCountStore); REG(cellSpursGetWorkloadInfo);
    REG(cellSpursShutdownWorkload); REG(cellSpursWaitForWorkloadShutdown); REG(cellSpursRemoveWorkload);
    REG(cellSpursAttachLv2EventQueue); REG(cellSpursDetachLv2EventQueue);
}
