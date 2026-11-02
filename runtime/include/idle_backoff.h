#pragma once
// Polling-loop detection for guest code that spins (an idle SPU job manager
// re-reading its queue). Shared by runtime.h users and the SPU headers, which
// do not include runtime.h. Kept free of standard headers: <chrono> brings in
// errno.h, whose EAGAIN/EINVAL macros collide with the kernel's error names.
#include <stdint.h>

/* One step of a wait loop: spin briefly (n = iterations so far), then sleep
   in short steps, so quick handoffs stay fast and long waits cost no CPU. */
void host_backoff(uint32_t n);
/* Monotonic time in nanoseconds (core.cpp). */
uint64_t host_now_ns();

struct IdleBackoff {
    uint32_t streak = 0;
    uint64_t last = 0;
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
        if (idle) host_backoff(streak++);
        else streak = 0;
    }
};
