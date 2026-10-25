// PPU threads: each guest thread is a host thread with its own PPUContext,
// guest stack and TLS block.
#include "runtime.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>

namespace thread {
namespace {
struct Thread {
    uint32_t id;
    PPUContext ctx;
    uint32_t entry_opd;
    std::string name;
    bool joinable;
    bool done = false;
    uint64_t status = 0;
    // for the watchdog: last OS entry point this thread called
    std::atomic<const char *> last_call{"(start)"};
    std::atomic<uint64_t> last_lr{0}, last_arg0{0};
    std::atomic<bool> in_call{false};
};

struct ThreadExit { uint64_t status; };

std::mutex g_lock;
std::condition_variable g_cv;
std::map<uint32_t, Thread *> g_threads;
std::atomic<uint32_t> g_next_id{0x01000000};
thread_local Thread *t_cur;

uint32_t g_tls_img, g_tls_filesz, g_tls_memsz;

constexpr SIZE_T HOST_STACK = 256u << 20; // generated frames are large and guest recursion is deep

void run(Thread *t) {
    t_cur = t;
    try {
        uint32_t fn = ld32(t->entry_opd);
        ppu_call(&t->ctx, fn);
        t->status = t->ctx.r[3];
    } catch (ThreadExit &e) {
        t->status = e.status;
    }
    LOG("thread", "'%s' exited (status 0x%llx)", t->name.c_str(), (unsigned long long)t->status);
    std::lock_guard<std::mutex> lk(g_lock);
    t->done = true;
    g_cv.notify_all();
}

DWORD WINAPI host_entry(LPVOID p) {
    run((Thread *)p);
    return 0;
}

Thread *make(uint32_t entry_opd, uint64_t arg, uint32_t stack, const char *name, bool joinable) {
    auto *t = new Thread();
    t->id = g_next_id++;
    t->entry_opd = entry_opd;
    t->name = name ? name : "";
    t->joinable = joinable;
    memset(&t->ctx, 0, sizeof t->ctx);
    if (stack < 0x4000) stack = 0x4000;
    uint32_t sp = mem::alloc_stack(stack);
    t->ctx.r[1] = (sp + stack - 0x70) & ~15u;
    t->ctx.r[2] = ld32(entry_opd + 4);
    t->ctx.r[3] = arg;
    t->ctx.r[13] = alloc_tls() + 0x7030;
    t->ctx.thread_id = t->id;
    t->ctx.reserve_addr = ~0ull;
    t->ctx.cr[0] = 0;
    std::lock_guard<std::mutex> lk(g_lock);
    g_threads[t->id] = t;
    return t;
}
} // namespace

void note_call(const char *name, PPUContext *ctx, bool entering) {
    if (!t_cur) return;
    if (entering) {
        t_cur->last_call = name;
        t_cur->last_lr = ctx->lr;
        t_cur->last_arg0 = ctx->r[3];
    }
    t_cur->in_call = entering;
}

void dump_all() {
    std::lock_guard<std::mutex> lk(g_lock);
    for (auto &[id, t] : g_threads) {
        LOG("watchdog", "  0x%x %-28s %s %s(0x%llx) lr=0x%llx", id, t->name.c_str(), t->done ? "DONE" : t->in_call ? "IN  " : "last",
            t->last_call.load(), (unsigned long long)t->last_arg0, (unsigned long long)t->last_lr);
        // TEMP: main thread polling a sound stream (f_009ad268): r29 = stream index.
        if (t->name == "main" && t->last_lr == 0x9ad294) {
            uint32_t idx = (uint32_t)t->ctx.r[29], toc = (uint32_t)t->ctx.r[2];
            uint32_t s = ld32(ld32(toc - 17240)) + idx * 14552;
            LOG("watchdog", "    stream %u @0x%x: +428=%x +432(state)=%x +436=%x +440=%x +444=%x +448=%x +452=%x +308=%x",
                idx, s, ld32(s + 428), ld32(s + 432), ld32(s + 436), ld32(s + 440), ld32(s + 444), ld32(s + 448),
                ld32(s + 452), ld32(s + 308));
            LOG("watchdog", "    service: count=%u array=0x%x (main array 0x%x)", ld32(ld32(toc - 16712)), ld32(ld32(toc - 16728)), ld32(ld32(toc - 17240)));
            uint32_t slot = s + 544 + ld32(s + 488) * 824;
            LOG("watchdog", "    slot %u @0x%x: +312(state)=%x +260(handle)=%x +256(reader)=%x +804=%x +276=%x", ld32(s + 488), slot,
                ld32(slot + 312), ld32(slot + 260), ld32(slot + 256), ld32(slot + 804), ld32(slot + 276));
        }
    }
}

// WORKAROUND: the main thread can wait forever in f_009ad268 for a sound
// stream's busy flag (+448) to clear, which normally happens when the voice
// fades out (f_009b6d18) - that path doesn't run yet. After 3 s of waiting
// on the same stream, clear the flag ourselves. KZ2_NO_SOUND_HACK disables.
void unstick_sound() {
    static uint32_t waiting_on = ~0u;
    static int ticks;
    std::lock_guard<std::mutex> lk(g_lock);
    for (auto &[id, t] : g_threads) {
        if (t->name != "main") continue;
        if (t->done || !t->in_call || t->last_lr != 0x9ad294) { waiting_on = ~0u; ticks = 0; return; }
        uint32_t idx = (uint32_t)t->ctx.r[29];
        if (idx != waiting_on) { waiting_on = idx; ticks = 0; return; }
        if (++ticks < 3) return;
        uint32_t s = ld32(ld32((uint32_t)t->ctx.r[2] - 17240)) + idx * 14552;
        LOG("sound", "unsticking stream %u (state %u, busy %u)", idx, ld32(s + 432), ld32(s + 448));
        st32(s + 448, 0);
        st32(s + 432, getenv("KZ2_UNSTICK_STATE") ? atoi(getenv("KZ2_UNSTICK_STATE")) : 0);
        ticks = 0;
        return;
    }
}

// WORKAROUND: the game's Job Manager queues SPU jobs that we never run (only
// Edge Zlib is native), and threads poll until a queue drains. Mark every
// queued job as consumed by moving each SPU's read cursor up to the write
// position. Job side effects (audio decode etc.) are skipped.
// Queue table: base = [[toc-16476]]; queue q header = base+64+q*16 (u16 put,
// then 7 u16 SPU cursors); ring = [base+48+q*4], 8-byte entries, u16 at +2
// nonzero = written job. KZ2_NO_JOB_SKIP disables.
void skip_spu_jobs() {
    uint32_t toc = 0;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        for (auto &[id, t] : g_threads)
            if (t->name == "main") toc = (uint32_t)t->ctx.r[2];
    }
    if (!toc) return;
    uint32_t jm = ld32(toc - 16476);
    if (!jm || !(jm = ld32(jm)) || !ld32(jm)) return;
    uint32_t base = ld32(jm);
    for (int q = 0; q < 4; q++) {
        uint32_t hdr = base + 64 + q * 16, ring = ld32(base + 48 + q * 4);
        if (!ring) continue;
        uint16_t put = (uint16_t)(ld32(hdr) >> 16);
        while ((ld32(ring + put * 8) & 0xffff) != 0) put++;
        for (int i = 1; i < 8; i++) {
            uint32_t a = hdr + 2 * i;
            uint32_t w = ld32(a & ~3u);
            w = (a & 2) ? (w & 0xffff0000u) | put : (w & 0xffffu) | ((uint32_t)put << 16);
            st32(a & ~3u, w);
        }
    }
}

uint32_t current_id() { return t_cur ? t_cur->id : 0; }
PPUContext *current() { return t_cur ? &t_cur->ctx : nullptr; }

void set_tls_params(uint32_t img, uint32_t filesz, uint32_t memsz) {
    g_tls_img = img, g_tls_filesz = filesz, g_tls_memsz = memsz;
}

uint32_t alloc_tls() {
    // 0x30-byte system area, then the TLS image. r13 = image + 0x7030.
    uint32_t base = mem::alloc(0x30 + g_tls_memsz + 16, 16);
    uint32_t img = base + 0x30;
    memset(mem::ptr(base), 0, 0x30 + g_tls_memsz);
    if (g_tls_filesz) memcpy(mem::ptr(img), mem::ptr(g_tls_img), g_tls_filesz);
    return img;
}

uint64_t create(uint32_t entry_opd, uint64_t arg, int prio, uint32_t stack, const char *name, bool joinable) {
    Thread *t = make(entry_opd, arg, stack, name, joinable);
    LOG("thread", "create '%s' id=0x%x entry=0x%x arg=0x%llx prio=%d stack=0x%x", t->name.c_str(), t->id,
        ld32(entry_opd), (unsigned long long)arg, prio, stack);
    HANDLE h = CreateThread(nullptr, HOST_STACK, host_entry, t, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) fatal("CreateThread failed");
    CloseHandle(h);
    return t->id;
}

void exit_current(uint64_t status) { throw ThreadExit{status}; }

int join(uint64_t id, uint64_t *status) {
    std::unique_lock<std::mutex> lk(g_lock);
    auto it = g_threads.find((uint32_t)id);
    if (it == g_threads.end()) return -1;
    Thread *t = it->second;
    g_cv.wait(lk, [&] { return t->done; });
    if (status) *status = t->status;
    return 0;
}

PPUContext *create_context() {
    auto *c = new PPUContext();
    memset(c, 0, sizeof *c);
    c->reserve_addr = ~0ull;
    return c;
}

void run_main(uint32_t argv, uint32_t envp);
} // namespace thread

#include "game_config.h"

void thread::run_main(uint32_t argv, uint32_t envp) {
    // The primary thread: registers as the PS3 loader sets them up for crt0.
    set_tls_params(GAME_TLS_ADDR, GAME_TLS_FILESZ, GAME_TLS_MEMSZ);
    Thread *t = make(GAME_ENTRY_OPD, 1, GAME_PRIMARY_STACK, "main", true);
    PPUContext &c = t->ctx;
    c.r[3] = 1;            // argc
    c.r[4] = argv;
    c.r[5] = envp;
    c.r[6] = 0;
    c.r[7] = t->id;
    c.r[8] = GAME_TLS_ADDR;
    c.r[9] = GAME_TLS_FILESZ;
    c.r[10] = GAME_TLS_MEMSZ;
    c.r[11] = GAME_ENTRY_OPD;
    c.r[12] = GAME_MALLOC_PAGESIZE;
    c.r[13] = 0;           // crt0 calls sys_initialize_tls itself
    LOG("thread", "starting main thread at 0x%x (toc 0x%x)", GAME_ENTRY, GAME_TOC);
    HANDLE h = CreateThread(nullptr, HOST_STACK, host_entry, t, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    WaitForSingleObject(h, INFINITE);
}
