// SPURS custom workloads: the game registers its own SPU policy modules
// (e.g. the Job Manager) with cellSpursAddWorkloadWithAttribute and wakes
// them with cellSpursReadyCountStore. Like SPURS, which runs a workload on as
// many SPUs as its ready count and maximum contention allow, each workload gets
// several SPU instances (host thread + SpuThread each); instance k runs while the
// ready count is above k. Job chains are built for this: their policy modules
// take jobs from shared queues with atomic DMA. Each instance runs the module
// the way the SPURS kernel does: module at LS 0xA00, kernel context at LS 0x100, r3 = context,
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
#include <vector>

#define REG(n) hle_register(#n, hle_##n)

void spurs_connect_events(spu::SpuThread &s, uint32_t spurs);

namespace mem { void track_counts(uint64_t *protects, uint64_t *faults); }
static inline void YieldProcessorSpurs() { __builtin_ia32_pause(); }
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
    uint8_t ready_raw() const { return __atomic_load_n(ready_byte(), __ATOMIC_SEQ_CST); }
    // Experiment (KZ2_SPURS_BOOST="name:n,..."): while the game asks for any SPU for
    // this workload, run up to n instances - the job manager hands independent jobs
    // to whichever instances run it, so a workload the game gave one SPU (jm_anim)
    // spreads over more host cores.
    int boost = 0;
    uint8_t ready() const { uint8_t r = ready_raw(); return r && boost > r ? (uint8_t)boost : r; }
    std::atomic<int> running{0};          // instances inside the policy module
    // flip log statistics: time instances spent running SPU code, runs, ready count seen
    std::atomic<uint64_t> busy_ns{0}, runs{0}, idle_ns{0}, backoff_ns{0};
    std::atomic<uint64_t> short_runs{0}, short_ns{0}, wakes{0}, false_wakes{0}, spin_ns{0};   // runs under 20 us; cv wake-ups
    std::atomic<int> ready_max{0}, running_max{0};
    std::atomic<bool> shutdown{false};
    std::mutex m;
    // One wake signal per instance, and only instances that have work are woken: waking
    // them all for one job cost ~100,000 thread switches a second at the shore, a quarter
    // of the process's CPU time in the kernel. KZ2_SPURS_WAKE_ALL=1: wake every instance.
    static constexpr int MAX_INSTANCES = 8;
    std::condition_variable cv[MAX_INSTANCES];
    std::atomic<uint32_t> sleeping{0};                   // bit k: instance k waits on cv[k]
    void wake() {
        static const bool all = getenv("KZ2_SPURS_WAKE_ALL") && atoi(getenv("KZ2_SPURS_WAKE_ALL"));
        int r = ready();
        uint32_t want = shutdown || all ? ~0u : r >= 32 ? ~0u : (1u << r) - 1;
        uint32_t targets = sleeping.load(std::memory_order_seq_cst) & want;
        if (!targets) return;
        { std::lock_guard<std::mutex> wl(m); }           // a waiter is either past its check or asleep
        for (int k = 0; k < MAX_INSTANCES; k++)
            if (targets >> k & 1) cv[k].notify_one();
    }
    std::vector<std::unique_ptr<spu::SpuThread>> spus;   // one per instance
};

// SPU instances per workload: the game's SPURS instance has 5 SPUs (KZ2_SPURS_SPUS
// overrides; 1 = the old single-instance behaviour), capped by maxContention.
int instances_for(uint32_t max_contention) {
    static const int n = [] {
        const char *e = getenv("KZ2_SPURS_SPUS");
        int v = e ? atoi(e) : 5;
        // At most the SPURS instance's SPU count (Killzone 2: 5, cellSpursInitialize*):
        // SPURS keeps per-SPU state for that many, and a 6th instance corrupts it
        // (tried 8: the game froze while loading).
        return v < 1 ? 1 : v > 5 ? 5 : v;
    }();
    int m = max_contention ? (int)max_contention : n;
    return std::min(n, m);
}

std::mutex g_lock;
std::map<uint32_t, std::unique_ptr<Workload>> g_workloads;
uint32_t g_next_wid = 0;

Workload *find(uint32_t wid) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_workloads.find(wid);
    return it == g_workloads.end() ? nullptr : it->second.get();
}

void init_context(Workload &w, spu::SpuThread &s, uint32_t spu_num) {
    // SpursKernelContext fields the policy module reads.
    s.ls_written(CTX_ADDR, 0x100);
    memset(s.ls + CTX_ADDR, 0, 0x100);
    s.set_ls32(CTX_ADDR + 0xc0, 0), s.set_ls32(CTX_ADDR + 0xc4, w.spurs);    // 0x1C0: spurs (u64)
    s.set_ls32(CTX_ADDR + 0xc8, spu_num);                                     // 0x1C8: spuNum
    s.set_ls32(CTX_ADDR + 0xcc, 31);                                          // 0x1CC: dmaTagId
    s.set_ls32(CTX_ADDR + 0xd0, 0), s.set_ls32(CTX_ADDR + 0xd4, w.pm);       // 0x1D0: wklCurrentAddr
    s.set_ls32(CTX_ADDR + 0xd8, w.size);                                      // 0x1D8: wklCurrentUniqueId / size
    s.set_ls32(CTX_ADDR + 0xdc, w.id);                                        // 0x1DC: wklCurrentId
    s.set_ls32(CTX_ADDR + 0xe0, EXIT_ADDR);                                   // 0x1E0: exitToKernelAddr
    s.set_ls32(CTX_ADDR + 0xe4, SELECT_WORKLOAD_ADDR);                        // 0x1E4: selectWorkloadAddr
}

// Runs the policy module whenever the workload has work (ready count > 0).
void workload_thread(Workload *w, int k) {
    auto &s = *w->spus[k];
    s.name = "spurs:" + (w->name.empty() ? std::to_string(w->id) : w->name) + (k ? "#" + std::to_string(k) : "");
    name_host_thread(s.name);
    if (const char *only = getenv("KZ2_SPU_TRACE_NAME"))
        if (s.name.find(only) == std::string::npos) s.traced = (int64_t)1 << 40;   // exclude from tracing
    memcpy(s.ls + PM_ADDR, mem::ptr(w->pm), w->size);
    s.code_loaded(PM_ADDR, w->pm, w->size);         // native code for the policy module, if compiled
    s.set_trap(SELECT_WORKLOAD_ADDR);
    s.set_trap(EXIT_ADDR);
    // Idle detection (see IdleBackoff): an idle job manager calls "select workload"
    // in a tight loop, or exits and is restarted at once; quick repeats back off.
    using clock = std::chrono::steady_clock;
    auto trap_idle = std::make_shared<IdleBackoff>();
    s.on_trap = [w, trap_idle](spu::SpuThread &t) {
        if (t.pc == EXIT_ADDR) return false;            // module returned to the kernel
        trap_idle->poll(true);                           // idle only if called again at once
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
    if (k == 0)
        LOG("spurs", "workload %u '%s': policy module 0x%x (%u bytes), arg 0x%llx, %zu SPU instances", w->id,
            w->name.c_str(), w->pm, w->size, (unsigned long long)w->data, w->spus.size());
    uint64_t runs = 0;
    IdleBackoff exit_idle;
    while (!w->shutdown) {
        // Short spin before sleeping: jobs arrive in bursts tens of microseconds apart, and a
        // kernel sleep and wake-up per job costs more than that (a quarter of the process's
        // CPU time, and latency along the whole job chain). 50 us: no gain while SPU code was slow,
        // but with faster code busy scenes went from ~38 to ~45 fps. KZ2_SPURS_SPIN_US=0: off.
        static const uint32_t spin_us = getenv("KZ2_SPURS_SPIN_US") ? (uint32_t)atoi(getenv("KZ2_SPURS_SPIN_US")) : 50;
        if (spin_us && runs) {
            uint64_t t0 = host_now_ns(), limit = spin_us * 1000ull;
            while (w->ready() <= k && !w->shutdown && host_now_ns() - t0 < limit)
                for (int p = 0; p < 8; p++) YieldProcessorSpurs();
            w->spin_ns.fetch_add(host_now_ns() - t0, std::memory_order_relaxed);
        }
        {
            std::unique_lock<std::mutex> lk(w->m);
            // Sleep until there is work for this instance. The ready count changes by SPU
            // atomics, which notify no one, so ready_monitor() polls it for everyone (one
            // thread instead of each instance polling: libc++'s wait_for rounds short
            // timeouts down to 0 ms on Windows, so a 1 ms poll loop mostly spins).
            uint64_t t_idle = host_now_ns();
            w->sleeping.fetch_or(1u << k, std::memory_order_seq_cst);
            bool checked = false;   // the check before sleeping is not a wake-up
            w->cv[k].wait(lk, [w, k, &checked] {
                bool go = w->ready() > k || w->shutdown;
                if (checked) {
                    w->wakes.fetch_add(1, std::memory_order_relaxed);
                    if (!go) w->false_wakes.fetch_add(1, std::memory_order_relaxed);
                }
                checked = true;
                return go;
            });
            w->sleeping.fetch_and(~(1u << k), std::memory_order_seq_cst);
            w->idle_ns += host_now_ns() - t_idle;
        }
        if (w->shutdown) break;
        if (w->ready() <= k) continue;
        if (k && runs == 0) LOG("spurs", "%s: extra SPU instance running (ready %d)", s.name.c_str(), w->ready());
        init_context(*w, s, k);
        for (auto &g : s.gpr) g = spu::V{};
        s.gpr[1] = spu::V{{0x3fff0, 0, 0, 0}};           // stack pointer at the top of LS
        s.gpr[3] = spu::V{{CTX_ADDR, 0, 0, 0}};
        s.gpr[4] = spu::V{{(uint32_t)(w->data >> 32), (uint32_t)w->data, 0, 0}};
        s.pc = PM_ADDR;
        s.interrupts = false, s.event_mask = 0, s.event_stat = 0, s.dec_armed = false, s.stall_stat = 0;
        for (auto &l : s.stalled) l.active = false;
        if (w->running++ == 0) gcm_tl("%s starts (ready %d)", w->name.c_str(), w->ready());
        { mem::LineGuard g(w->spurs); __atomic_add_fetch(w->contention_byte(), 1, __ATOMIC_SEQ_CST); }
        // Log the first few runs of each workload, then every 1000th.
        bool log = runs < 5 || runs % 1000 == 0;
        if (log) LOG("spurs", "%s: run #%llu (ready %d)", s.name.c_str(), (unsigned long long)runs, w->ready());
        auto t_run = clock::now();
        w->runs++;
        {
            int r = w->ready(), m = w->ready_max.load();
            while (r > m && !w->ready_max.compare_exchange_weak(m, r)) {}
            int u = w->running.load(), mu = w->running_max.load();
            while (u > mu && !w->running_max.compare_exchange_weak(mu, u)) {}
        }
        s.run();
        {
            uint64_t ran = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t_run).count();
            w->busy_ns += ran;
            if (ran < 20000) w->short_runs.fetch_add(1, std::memory_order_relaxed), w->short_ns.fetch_add(ran, std::memory_order_relaxed);
        }
        w->backoff_ns += take_backoff_ns();   // of busy: sleeping in polling loops (empty queue, locks)
        if (log) LOG("spurs", "%s: exited at 0x%x", s.name.c_str(), s.pc);
        exit_idle.step(clock::now() - t_run < std::chrono::microseconds(30));   // found nothing to do
        runs++;
        { mem::LineGuard g(w->spurs); __atomic_sub_fetch(w->contention_byte(), 1, __ATOMIC_SEQ_CST); }
        if (--w->running == 0) gcm_tl("%s idle", w->name.c_str());
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
// An atomic commit to a SPURS instance's first line, where the ready counts live (SPU
// job managers and libspurs change them with atomics, which notify no one): wake that
// instance's workloads that now want more SPUs than run. Without this, waiting
// instances only noticed new work at the ready monitor's next poll - ~0.3-0.5 ms per
// hand-off, hundreds of times a frame in a chain of dependent post-processing jobs.
// KZ2_SPURS_WAKE_ON_WRITE=0: monitor only.
namespace { std::atomic<uint32_t> g_spurs_lines[8]; Workload *g_wl_list[64]; std::atomic<int> g_wl_count{0}; }
extern "C" void line_notify(uint64_t ea) {
    line_wake(ea);
    uint32_t line = (uint32_t)ea & ~127u;
    bool ours = false;
    for (auto &l : g_spurs_lines) if (l.load(std::memory_order_relaxed) == line) ours = true;
    if (!ours) return;
    // No lock: this runs on every atomic commit to the line, from about 25 threads. The
    // list only grows and workloads stay allocated.
    int n = g_wl_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++) {
        Workload *w = g_wl_list[i];
        if ((w->spurs & ~127u) == line && w->ready() > w->running) w->wake();
    }
}

// KZ2_CHAIN_STAT=1: what runs while the RSX is parked, sampled about every 50 us (burns one
// core): how many jm_post instances are inside the job manager, and when none is, which other
// workloads are. Reported in the stats line.
namespace {
std::atomic<uint64_t> g_cs_parked{0}, g_cs_post[9], g_cs_none_any{0}, g_cs_none_other[64];
void chain_sampler() {
    name_host_thread("chain sampler");
    for (;;) {
        host_sleep_us(50);
        if (!gcm_parked_ea()) continue;
        g_cs_parked++;
        int n = g_wl_count.load(), post = 0;
        for (int i = 0; i < n; i++)
            if (g_wl_list[i]->name == "jm_post") post = g_wl_list[i]->running.load();
        g_cs_post[std::min(post, 8)]++;
        if (post) continue;
        bool any = false;
        for (int i = 0; i < n; i++)
            if (g_wl_list[i]->running.load() > 0) any = true, g_cs_none_other[i]++;
        if (!any) g_cs_none_any++;
    }
}
} // namespace

// Per workload since the last call: "name busy ms/frame (runs/frame, ready max r,
// instances used u/n)". Resets.
std::string spurs_stats_line(double frames) {
    std::string out;
    {
        static double last_k, last_u;
        double k, u;
        process_cpu_ms(&k, &u);
        char pb[160];
        static uint64_t last_p, last_f;
        uint64_t tp, tf;
        mem::track_counts(&tp, &tf);
        snprintf(pb, sizeof pb, "process CPU kernel %.0f ms, user %.0f ms, page protects %.0f, write faults %.0f; ",
                 (k - last_k) / frames, (u - last_u) / frames, (tp - last_p) / frames, (tf - last_f) / frames);
        last_p = tp, last_f = tf;
        last_k = k, last_u = u;
        out = pb;
    }
    static const bool chain = getenv("KZ2_CHAIN_STAT") != nullptr;
    if (chain) {
        static std::once_flag once;
        std::call_once(once, [] { std::thread(chain_sampler).detach(); });
        uint64_t parked = g_cs_parked.exchange(0);
        if (parked) {
            char cb[200];
            int o = snprintf(cb, sizeof cb, "while parked (%.1f ms/frame) jm_post running:", parked * 0.05 / frames);
            for (int i = 0; i < 6; i++) o += snprintf(cb + o, sizeof cb - o, " %d=%.0f%%", i, 100.0 * g_cs_post[i].exchange(0) / parked);
            out += cb;
            o = snprintf(cb, sizeof cb, "; none of post, nothing at all %.0f%%; others:", 100.0 * g_cs_none_any.exchange(0) / parked);
            out += cb;
            int wn = g_wl_count.load();
            for (int i = 0; i < wn; i++)
                if (uint64_t v = g_cs_none_other[i].exchange(0)) {
                    snprintf(cb, sizeof cb, " %s=%.0f%%", g_wl_list[i]->name.c_str(), 100.0 * v / parked);
                    out += cb;
                }
            out += "; ";
        }
    }
    size_t first = out.size();
    std::lock_guard<std::mutex> lk(g_lock);
    for (auto &[id, w] : g_workloads) {
        uint64_t b = w->busy_ns.exchange(0), r = w->runs.exchange(0), idl = w->idle_ns.exchange(0), bo = w->backoff_ns.exchange(0);
        int rm = w->ready_max.exchange(0), um = w->running_max.exchange(0);
        if (!b && !r) continue;
        uint64_t sr = w->short_runs.exchange(0), sn = w->short_ns.exchange(0), wk = w->wakes.exchange(0), fw = w->false_wakes.exchange(0), sp = w->spin_ns.exchange(0);
        char buf[320];
        snprintf(buf, sizeof buf, "%s%s run %.1f ms (of it backoff sleeps %.1f), between runs %.1f ms (%.0f runs, %.0f under 20 us = %.1f ms; %.0f wake-ups, %.0f false, spin %.1f ms; ready max %d, instances %d/%zu)",
                 out.size() == first ? "" : "; ", w->name.c_str(), b / 1e6 / frames, bo / 1e6 / frames, idl / 1e6 / frames, r / frames,
                 sr / frames, sn / 1e6 / frames, wk / frames, fw / frames, sp / 1e6 / frames, rm, um, w->spus.size());
        out += buf;
    }
    return out;
}

// Wakes SPU instances: one thread checks every workload each millisecond and
// notifies those asking for more SPUs (ready count) than are running.
void ready_monitor() {
    name_host_thread("spurs ready monitor");
    for (;;) {
        // Commits wake instances directly (line_notify): this only catches what they miss.
        static const uint32_t period = getenv("KZ2_SPURS_MONITOR_US") ? (uint32_t)atoi(getenv("KZ2_SPURS_MONITOR_US")) : 1000;
        host_sleep_us(period);
        std::lock_guard<std::mutex> lk(g_lock);
        for (auto &[id, w] : g_workloads)
            if (w->ready() > w->running) w->wake();
    }
}

HLE(cellSpursAddWorkloadWithAttribute) {
    // (spurs, u32 *wid, attr)
    static std::once_flag monitor;
    std::call_once(monitor, [] { std::thread(ready_monitor).detach(); });
    uint32_t a = ARG32(2);
    auto w = std::make_unique<Workload>();
    w->spurs = ARG32(0);
    w->pm = ld32(a + A_PM), w->size = ld32(a + A_SIZE), w->data = ld64(a + A_DATA);
    if (ld32(a + A_INSTANCE)) w->name = mem::cstr(ld32(a + A_INSTANCE));
    int n = instances_for(ld32(a + A_MAXC));
    for (int k = 0; k < n; k++) w->spus.push_back(std::make_unique<spu::SpuThread>());
    Workload *p = w.get();
    {
        std::lock_guard<std::mutex> lk(g_lock);
        p->id = g_next_wid++;
        g_workloads[p->id] = std::move(w);
        int wn = g_wl_count.load();
        if (wn < 64) g_wl_list[wn] = p, g_wl_count.store(wn + 1, std::memory_order_release);
    }
    st32(ARG32(1), p->id);
    static const bool wake_on_write = !getenv("KZ2_SPURS_WAKE_ON_WRITE") || atoi(getenv("KZ2_SPURS_WAKE_ON_WRITE"));
    if (wake_on_write) {
        uint32_t line = p->spurs & ~127u;
        bool have = false;
        for (auto &l : g_spurs_lines) if (l.load() == line) have = true;
        for (auto &l : g_spurs_lines) {
            uint32_t z = 0;
            if (have || l.compare_exchange_strong(z, line)) break;
        }
        g_line_notify[line_index(line)] = 1;
    }
    // Default: jm_anim (animation, which the main thread waits for) on up to 4 instances;
    // KZ2_SPURS_BOOST="" disables, or lists other "name:n" pairs.
    const char *boost_env = getenv("KZ2_SPURS_BOOST");
    if (!boost_env) boost_env = "jm_anim:4";
    if (const char *e = boost_env)
        for (const char *q = e; *q;) {
            const char *c = strchr(q, ':'), *end = strchr(q, ',');
            if (!end) end = q + strlen(q);
            if (c && c < end && p->name == std::string(q, c - q)) {
                p->boost = std::min(atoi(c + 1), n);
                LOG("spurs", "workload '%s': boosted to %d instances while ready", p->name.c_str(), p->boost);
            }
            q = *end ? end + 1 : end;
        }
    for (int k = 0; k < n; k++) std::thread(workload_thread, p, k).detach();
    RET(0);
}
HLE(cellSpursReadyCountStore) {
    // (spurs, wid, value)
    Workload *w = find(ARG32(1));
    if (!w) { RET(0x80410711); return; }                 // CELL_SPURS_CORE_ERROR_INVAL
    static std::atomic<int> logged{0};
    if (logged++ < 30) LOG("spurs", "ready count of workload %u '%s' = %d", w->id, w->name.c_str(), (int32_t)ARG32(2));
    { mem::LineGuard g(w->spurs); __atomic_store_n(w->ready_byte(), (uint8_t)ARG32(2), __ATOMIC_SEQ_CST); }
    w->wake();
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
    if (Workload *w = find(ARG32(1))) { w->shutdown = true; w->wake(); }
    RET(0);
}
HLE(cellSpursWaitForWorkloadShutdown) {
    if (Workload *w = find(ARG32(1)))
        for (uint32_t n = 0; w->running; n++) host_backoff(n);
    RET(0);
}
HLE(cellSpursRemoveWorkload) { RET(0); }

void register_spurs_kernel() {
    REG(_cellSpursWorkloadAttributeInitialize); REG(cellSpursWorkloadAttributeSetName);
    REG(cellSpursAddWorkloadWithAttribute); REG(cellSpursReadyCountStore); REG(cellSpursGetWorkloadInfo);
    REG(cellSpursShutdownWorkload); REG(cellSpursWaitForWorkloadShutdown); REG(cellSpursRemoveWorkload);
    REG(cellSpursAttachLv2EventQueue); REG(cellSpursDetachLv2EventQueue);
}
