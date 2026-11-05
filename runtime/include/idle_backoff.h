#pragma once
// Polling-loop detection for guest code that spins (an idle SPU job manager
// re-reading its queue). Shared by runtime.h users and the SPU headers, which
// do not include runtime.h. Kept free of standard headers: <chrono> brings in
// errno.h, whose EAGAIN/EINVAL macros collide with the kernel's error names.
#include <stdint.h>

/* One step of a wait loop: spin briefly (n = iterations so far), then sleep
   in short steps, so quick handoffs stay fast and long waits cost no CPU. */
void host_backoff(uint32_t n);
/* HELSARI_FAST_WAKE=1 experiment: short sleeps only (core.cpp). */
bool fast_wake();
/* Time the calling thread spent sleeping in host_backoff since the last call (core.cpp). */
uint64_t take_backoff_ns();
/* Monotonic time in nanoseconds (core.cpp). */
uint64_t host_now_ns();
/* Process CPU time so far, in ms (core.cpp). */
void process_cpu_ms(double *kernel, double *user);
/* Measured waste: time in idle polling streaks, and backoff calls per phase (core.cpp). */
void add_idle_poll_ns(uint64_t ns);
void idle_poll_stats(uint64_t *poll_ns, uint64_t *pauses, uint64_t *yields, uint64_t *sleeps);

struct IdleBackoff {
    uint32_t streak = 0;
    uint64_t last = 0, streak_start = 0;
    // One iteration of a guest polling loop. It counts as idle only if nothing
    // changed AND it came right after the previous iteration - no guest work in
    // between (the time spent in our own backoff is excluded). Consecutive idle
    // iterations back off from pausing to real sleeps; anything else resets.
    void poll(bool unchanged, uint64_t max_gap_ns = 10000) {
        bool idle = unchanged && host_now_ns() - last < max_gap_ns;
        step(idle);
        last = host_now_ns();
    }
    // For callers that judge idleness themselves (e.g. a run that returned at once).
    void step(bool idle) {
        if (idle) {
            if (!streak) streak_start = host_now_ns();
            host_backoff(streak++);
        } else {
            if (streak) add_idle_poll_ns(host_now_ns() - streak_start);
            streak = 0;
        }
    }
};
