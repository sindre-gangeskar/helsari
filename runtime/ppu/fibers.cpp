// The game's microthread scheduler, on host fibers.
//
// The game runs some work as cooperative microthreads. A context buffer
// holds the callee-saved registers, r1 (+144), r2 (+152), the resume address
// (+160, the saved r0), r3 (+168) and r4/r5 (+512/+520):
//   0x72988  save(buf)                 - like setjmp
//   0x731f8  pick(scheduler) -> buf    - next runnable context
//   0x72a9c  restore(buf)              - loads it and "returns" into it
//   0x72bb4  yield(cur, scheduler)     - save(cur); restore(pick())
// New contexts are made by saving a template and patching r1 (fresh stack),
// r2 and the resume address (an entry point). Restoring a context continues
// on that context's stack, but generated code keeps its frames on the host
// stack, so every guest context runs on its own host fiber: yield suspends
// the current fiber, and restore resumes the fiber suspended in that buffer
// or starts a new one at the buffer's entry point.
#include "runtime.h"
#include "idle_backoff.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <map>
#include <mutex>

extern "C" {
void f_00072988(PPU_CTX);           // save
void f_000731f8(PPU_CTX);           // pick
void f_00072a9c_guest(PPU_CTX);     // restore: the generated register loads
}

namespace {

constexpr SIZE_T FIBER_STACK = 256u << 20;   // as for guest threads: generated frames are large
enum { CTX_R1 = 144, CTX_PC = 160 };

struct Fiber {
    void *handle = nullptr;
    uint32_t buf = 0;          // context buffer it is suspended in (or starts from)
    uint64_t sp = 0;           // r1 and resume address saved there, to tell a
    uint32_t pc = 0;           // suspended fiber from a re-initialized buffer
    bool primary = false;      // the guest thread's own fiber: deleting it would end the thread
};

std::mutex g_lock;
std::map<uint32_t, Fiber *> g_suspended;      // buffer -> fiber that yielded into it
std::atomic<int64_t> g_live{0};               // fibers created and not yet deleted (flip log)

thread_local Fiber *t_cur;                    // fiber running on this thread
thread_local Fiber *t_dead;                   // abandoned fiber, deleted by the next one to run
thread_local PPUContext *t_ctx;               // guest registers handed across a switch

void reap() {
    if (Fiber *d = t_dead) {
        t_dead = nullptr;
        if (d->primary) return;               // stays converted; never resumed again
        DeleteFiber(d->handle);
        delete d;
        g_live--;
    }
}

Fiber *current() {
    if (!t_cur) {                             // first switch on this thread: adopt the thread itself
        t_cur = new Fiber();
        t_cur->primary = true;
        t_cur->handle = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(nullptr);
        if (!t_cur->handle) fatal("ConvertThreadToFiber failed (%lu)", GetLastError());
    }
    return t_cur;
}

void CALLBACK fresh_entry(void *p) {
    Fiber *self = (Fiber *)p;
    reap();
    PPUContext *ctx = t_ctx;
    ctx->r[3] = self->buf;
    f_00072a9c_guest(ctx);                    // r1 = new stack, r2, r3 = argument, lr = entry
    uint32_t entry = (uint32_t)ctx->lr;
    ppu_call(ctx, entry);
    fatal("microthread entry 0x%x returned (context 0x%x)", entry, self->buf);
}

// Leave the current fiber for the context in `next`. If `suspend_in` is set
// the current fiber has saved itself there and returns from this call when
// that buffer is restored; otherwise it is abandoned.
void switch_to(PPUContext *ctx, uint32_t next, uint32_t suspend_in) {
    Fiber *me = current(), *target = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_suspended.find(next);
        if (it != g_suspended.end()) {
            Fiber *f = it->second;
            g_suspended.erase(it);
            if (f->sp == ld64(next + CTX_R1) && f->pc == ld32(next + CTX_PC)) target = f;
            else t_dead = f;                  // buffer was re-initialized: that fiber is gone
        }
        if (suspend_in) {
            me->buf = suspend_in, me->sp = ld64(suspend_in + CTX_R1), me->pc = ld32(suspend_in + CTX_PC);
            g_suspended[suspend_in] = me;
        }
    }
    if (t_dead) reap();
    if (!target) {
        target = new Fiber();
        target->buf = next;
        target->handle = CreateFiberEx(64 << 10, FIBER_STACK, FIBER_FLAG_FLOAT_SWITCH, fresh_entry, target);
        if (!target->handle) fatal("CreateFiberEx failed (%lu)", GetLastError());
        g_live++;
    }
    if (!suspend_in) t_dead = me;
    t_ctx = ctx;
    t_cur = target;
    SwitchToFiber(target->handle);
    // Resumed: someone restored our buffer.
    reap();
    if (t_ctx != ctx) fatal("microthread context 0x%x resumed on another guest thread", suspend_in);
}

} // namespace

extern "C" void f_00072bb4(PPU_CTX) {
    // yield(cur = r3, scheduler = r4)
    uint32_t cur = (uint32_t)ctx->r[3], sched = (uint32_t)ctx->r[4];
    ctx->r[0] = ctx->lr;                      // the guest's mflr r0: save() stores it as the resume address
    f_00072988(ctx);
    ctx->r[3] = sched;
    f_000731f8(ctx);
    uint32_t next = (uint32_t)ctx->r[3];
    if (next != cur) switch_to(ctx, next, cur);
    ctx->r[3] = cur;
    f_00072a9c_guest(ctx);                    // our registers; lr = our caller
}

// pick(scheduler = r3) -> context buffer. Same logic as the guest code (0x731f8):
// five slots, tried round-robin after the current one (scheduler +12); a slot is
// in use when its word at +3884 (slots are 4 bytes apart from scheduler[+8]) is
// non-zero, and runnable when its wait pointer (+3864) is null or points to a
// non-zero word. No slot in use: current = -1, return the idle context (+0xc80).
// The guest loops without pause while slots exist but none is runnable - a
// busy-wait for an SPU job or another thread to set a word, which kept a host
// core spinning in the main thread. Here each fruitless pass backs off (pause,
// yield, then 50 us sleeps). HELSARI_PICK_SPIN=1: the original guest code.
extern "C" void f_000731f8_guest(PPU_CTX);
extern "C" void f_000731f8(PPU_CTX) {
    static const bool spin = getenv("HELSARI_PICK_SPIN") && atoi(getenv("HELSARI_PICK_SPIN"));
    if (spin) { f_000731f8_guest(ctx); return; }
    const uint32_t s = (uint32_t)ctx->r[3];
    uint64_t t_wait = 0;   // set at the first fruitless pass: time waited goes to the flip log
    struct Account { uint64_t &t; ~Account() { if (t) thread::add_wait("microthread wait", host_now_ns() - t); } } acc{t_wait};
    for (uint32_t pass = 0;; pass++) {
        bool any = false;
        for (int32_t k = 0; k < 5; k++) {
            int32_t i = ((int32_t)ld32(s + 12) + k + 1) % 5;
            uint32_t slot = ld32(s + 8) + (uint32_t)i * 4;
            if (!ld32(slot + 3884)) continue;
            any = true;
            uint32_t w = ld32(slot + 3864);
            if (w) {
                std::atomic_thread_fence(std::memory_order_seq_cst);
                uint32_t v = ld32(w);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (!v) continue;
            }
            st32(s + 12, (uint32_t)i);
            ctx->r[3] = (uint32_t)(ld32(s + 8) + (uint32_t)i * 640);
            return;
        }
        if (!any) {
            st32(s + 12, 0xffffffffu);
            ctx->r[3] = (uint32_t)(ld32(s + 8) + 0xc80);
            return;
        }
        if (!t_wait) t_wait = host_now_ns();
        host_backoff(pass < 1000 ? pass : 1000);
    }
}

extern "C" void f_00072a9c(PPU_CTX) {
    // restore(buf = r3) reached directly (microthread exit): switch for good.
    switch_to(ctx, (uint32_t)ctx->r[3], 0);
    fatal("abandoned microthread was resumed");
}

// Microthread fibers alive, and those parked in a context buffer (flip log).
std::string fiber_stats() {
    std::lock_guard<std::mutex> lk(g_lock);
    return std::to_string(g_live.load()) + " fibers (" + std::to_string(g_suspended.size()) + " suspended)";
}
