// Debugging hooks on game functions (see tools/recomp/overrides.py). Each
// logs what it is asked for, then runs the generated original.
#include "runtime.h"

#include <cstdlib>
#include <string>
#include <vector>

extern "C" void f_00a4a99c_guest(PPU_CTX);

// KZ2_HOOK_WATCH="ea:len,ea:len": hooks and host writers log only what overlaps.
bool mem::hook_watched(uint32_t ea, uint32_t size) {
    static const std::vector<std::pair<uint32_t, uint32_t>> ranges = [] {
        std::vector<std::pair<uint32_t, uint32_t>> r;
        if (const char *p = getenv("KZ2_HOOK_WATCH"))
            for (const char *s = p; *s;) {
                char *end;
                uint32_t a = strtoul(s, &end, 16), n = *end == ':' ? strtoul(end + 1, &end, 16) : 4;
                r.push_back({a, n});
                s = *end == ',' ? end + 1 : end;
            }
        return r;
    }();
    for (auto &[a, n] : ranges)
        if (ea < a + n && a < ea + size) return true;
    return false;
}

namespace {

std::string hex(uint32_t ea, int bytes) {
    std::string h;
    for (int k = 0; k < bytes; k += 4) {
        char b[12];
        snprintf(b, sizeof b, " %08x", ld32(ea + k));
        h += b;
    }
    return h;
}

} // namespace

// Buffered asset stream read: r3 = reader, r4 = destination, r5 = size. The
// reader's state is at [r3+12]: +176 read cursor, +180 end of buffered data;
// reads larger than what is buffered go through a refill path.
extern "C" void f_00a4a99c(PPU_CTX) {
    uint32_t dst = (uint32_t)ctx->r[4], size = (uint32_t)ctx->r[5];
    if (mem::hook_watched(dst, size)) {
        uint32_t st = ld32((uint32_t)ctx->r[3] + 12), cur = ld32(st + 176), end = ld32(st + 180);
        LOG("hook", "stream read 0x%x bytes -> 0x%x: reader 0x%x cursor 0x%x end 0x%x (%s); src:%s", size, dst,
            (uint32_t)ctx->r[3], cur, end, (int32_t)(end - cur) < (int32_t)size ? "refill" : "buffered",
            hex(cur, 32).c_str());
        f_00a4a99c_guest(ctx);
        LOG("hook", "  -> dst now:%s", hex(dst, 32).c_str());
        return;
    }
    f_00a4a99c_guest(ctx);
}
