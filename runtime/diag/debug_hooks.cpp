// Debugging hooks on game functions (see tools/recomp/overrides.py). Each
// logs what it is asked for, then runs the generated original.
#include "runtime.h"

#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

extern "C" void f_00a4a99c_guest(PPU_CTX);
void edge_zlib_explain(uint32_t ea, uint32_t size);

// HELSARI_HOOK_WATCH="ea:len,ea:len": hooks and host writers log only what overlaps.
bool mem::hook_watched(uint32_t ea, uint32_t size) {
    static const std::vector<std::pair<uint32_t, uint32_t>> ranges = [] {
        std::vector<std::pair<uint32_t, uint32_t>> r;
        if (const char *p = getenv("HELSARI_HOOK_WATCH"))
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
        // HELSARI_WATCH_WRITE_LATE="ea:len,...": write watches armed only once a watched
        // asset is read (e.g. after level load), so earlier traffic does not fill the log.
        static std::once_flag late;
        std::call_once(late, [] {
            if (const char *p = getenv("HELSARI_WATCH_WRITE_LATE"))
                for (const char *s = p; *s;) {
                    char *e;
                    uint32_t a = strtoul(s, &e, 16), n = *e == ':' ? strtoul(e + 1, &e, 16) : 4;
                    mem::watch_add(a, n);
                    s = *e == ',' ? e + 1 : e;
                }
        });
        uint32_t st = ld32((uint32_t)ctx->r[3] + 12), cur = ld32(st + 176), end = ld32(st + 180);
        LOG("hook", "stream read 0x%x bytes -> 0x%x: reader 0x%x cursor 0x%x end 0x%x (%s); src:%s", size, dst,
            (uint32_t)ctx->r[3], cur, end, (int32_t)(end - cur) < (int32_t)size ? "refill" : "buffered",
            hex(cur, 32).c_str());
        if ((int32_t)(end - cur) >= (int32_t)size) edge_zlib_explain(cur, size);
        f_00a4a99c_guest(ctx);
        LOG("hook", "  -> dst now:%s", hex(dst, 32).c_str());
        return;
    }
    f_00a4a99c_guest(ctx);
}

// The scripts' trace and print functions (one empty function in the shipped
// game): log their text arguments, so the menu scripts' progress shows in the
// log. HELSARI_NO_SCRIPT_TRACE=1 turns it off.
extern "C" void f_003e2630_guest(PPU_CTX);
extern "C" void f_003f0388(PPU_CTX);   // lua_gettop(L)
extern "C" void f_003f0980(PPU_CTX);   // lua_tolstring(L, index, len)

extern "C" void f_003e2630(PPU_CTX) {
    static const bool off = getenv("HELSARI_NO_SCRIPT_TRACE") != nullptr;
    if (!off) {
        const PPUContext saved = *ctx;
        const uint32_t L = (uint32_t)saved.r[3];
        ctx->r[1] = (saved.r[1] - 256) & ~15ull;
        st64(ctx->r[1], saved.r[1]);
        ctx->r[3] = L;
        f_003f0388(ctx);
        const int top = (int32_t)ctx->r[3];
        std::string line;
        for (int i = 1; i <= top && i <= 8; i++) {
            ctx->r[3] = L, ctx->r[4] = (uint64_t)i, ctx->r[5] = 0;
            f_003f0980(ctx);   // numbers convert in place; other types give no text
            const uint32_t s = (uint32_t)ctx->r[3];
            line += i > 1 ? " " : "";
            line += s ? std::string((const char *)mem::ptr(s)).substr(0, 300) : "?";
        }
        LOG("lua", "trace: %s", line.c_str());
        *ctx = saved;
    }
    f_003e2630_guest(ctx);
}

// Script SwitchToGameModule: logs the handler it calls (a descriptor stored at +76 of the module
// object that r3 points to), so the switch can be followed.
extern "C" void f_00077310_guest(PPU_CTX);
extern "C" void f_00077310(PPU_CTX) {
    const uint32_t obj = ld32((uint32_t)ctx->r[3]), opd = obj ? ld32(obj + 76) : 0;
    LOG("module", "SwitchToGameModule: object 0x%x, handler 0x%x (code 0x%x)", obj, opd, opd ? ld32(opd) : 0);
    f_00077310_guest(ctx);
    LOG("module", "SwitchToGameModule returned 0x%llx", (unsigned long long)ctx->r[3]);
}
