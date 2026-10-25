// Function lookup, indirect calls, HLE import binding, logging.
#include "runtime.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/* ---------------- logging ---------------- */
static std::mutex g_log_lock;
static FILE *g_log_file;

void log_msg(const char *chan, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_log_lock);
    if (!g_log_file) g_log_file = fopen("kz2.log", "w");
    unsigned tid = thread::current_id();
    fprintf(stdout, "[%s] t%u: %s\n", chan, tid, buf);
    if (g_log_file) { fprintf(g_log_file, "[%s] t%u: %s\n", chan, tid, buf); fflush(g_log_file); }
}

void fatal(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_msg("FATAL", "%s", buf);
    fflush(stdout);
    ExitProcess(1);
}

// Log the guest call chain: each frame's first doubleword links to the
// caller's frame, and the caller's return address is saved at that frame + 16.
static void backtrace(PPUContext *ctx) {
    std::string s;
    uint32_t sp = (uint32_t)ctx->r[1];
    for (int i = 0; i < 16 && sp && sp < 0xf0000000u; i++) {
        uint32_t caller = (uint32_t)ld64(sp);
        if (!caller || caller <= sp) break;
        char b[16];
        snprintf(b, sizeof b, " 0x%x", (uint32_t)ld64(caller + 16));
        s += b;
        sp = caller;
    }
    log_msg("FATAL", "backtrace (return addresses):%s", s.c_str());
    log_msg("FATAL", "r3=0x%llx r4=0x%llx r5=0x%llx r11=0x%llx r13=0x%llx r31=0x%llx",
            (unsigned long long)ctx->r[3], (unsigned long long)ctx->r[4], (unsigned long long)ctx->r[5],
            (unsigned long long)ctx->r[11], (unsigned long long)ctx->r[13], (unsigned long long)ctx->r[31]);
    // KZ2_FATAL_PEEK=addr,addr,...: words to show (prefix 'r13' for TLS-relative, e.g. r13-28068)
    if (const char *p = getenv("KZ2_FATAL_PEEK")) {
        std::string list = p;
        size_t i = 0;
        while (i < list.size()) {
            size_t e = list.find(',', i);
            std::string item = list.substr(i, e == std::string::npos ? std::string::npos : e - i);
            uint32_t a = item.rfind("r13", 0) == 0 ? (uint32_t)(ctx->r[13] + strtoll(item.c_str() + 3, nullptr, 0))
                                                   : (uint32_t)strtoull(item.c_str(), nullptr, 0);
            log_msg("FATAL", "  [%s] 0x%x = 0x%08x 0x%08x", item.c_str(), a, ld32(a), ld32(a + 4));
            if (e == std::string::npos) break;
            i = e + 1;
        }
    }
}

/* ---------------- function table ---------------- */
static std::vector<ppu_fn> g_table;   // indexed by (addr - ELF_BASE) / 4
static uint32_t g_table_end;

static void build_table() {
    uint32_t hi = g_funcs[g_func_count - 1].addr + 4;
    g_table_end = hi;
    g_table.assign((hi - mem::ELF_BASE) / 4, nullptr);
    for (uint32_t i = 0; i < g_func_count; i++) g_table[(g_funcs[i].addr - mem::ELF_BASE) / 4] = g_funcs[i].fn;
}

static ppu_fn lookup(uint32_t addr) {
    if (addr < mem::ELF_BASE || addr >= g_table_end || (addr & 3)) return nullptr;
    return g_table[(addr - mem::ELF_BASE) / 4];
}

extern "C" void ppu_call(PPUContext *ctx, uint32_t addr) {
    if (ppu_fn f = lookup(addr)) return f(ctx);
    if (hle_dispatch(ctx, addr)) return;
    backtrace(ctx);
    fatal("call to unknown guest address 0x%08x (lr=0x%llx)", addr, (unsigned long long)ctx->lr);
}

extern "C" void ppu_dispatch(PPUContext *ctx) {
    uint32_t t = ctx->jt;
    if (ppu_fn f = lookup(t)) { __attribute__((musttail)) return f(ctx); }
    if (hle_dispatch(ctx, t)) return;
    backtrace(ctx);
    fatal("indirect jump to 0x%08x is not a known function entry (lr=0x%llx) - "
          "probably an unresolved jump table; add it to the analyzer", t, (unsigned long long)ctx->lr);
}

extern "C" void ppu_unimpl(PPUContext *ctx, uint32_t addr, uint32_t word) {
    backtrace(ctx);
    fatal("reached untranslated code at 0x%08x (word %08x, lr=0x%llx)", addr, word, (unsigned long long)ctx->lr);
}

extern "C" void ppu_trap(PPUContext *ctx, uint32_t addr) {
    static std::atomic<int> n;
    if (n++ < 50) LOG("ppu", "trap instruction hit at 0x%08x (lr=0x%llx) - continuing", addr, (unsigned long long)ctx->lr);
}

extern "C" uint64_t ppu_timebase(void) {
    // PS3 timebase runs at 79.8 MHz
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)((unsigned __int128)c.QuadPart * 79800000ull / (uint64_t)freq.QuadPart);
}

extern "C" void ppu_lswi(PPUContext *ctx, int rd, uint32_t ea, int nb) {
    int r = rd;
    for (int i = 0; i < nb; i++) {
        int b = i & 3;
        if (!b) {
            if (i) r = (r + 1) & 31;
            ctx->r[r] = 0;
        }
        ctx->r[r] |= (uint64_t)ld8(ea + i) << (24 - 8 * b);
    }
}

extern "C" void ppu_stswi(PPUContext *ctx, int rs, uint32_t ea, int nb) {
    int r = rs;
    for (int i = 0; i < nb; i++) {
        int b = i & 3;
        if (i && !b) r = (r + 1) & 31;
        st8(ea + i, (uint8_t)(ctx->r[r] >> (24 - 8 * b)));
    }
}

void ppu_invoke(PPUContext *ctx, uint32_t opd) {
    uint64_t sp = ctx->r[1], toc = ctx->r[2], lr = ctx->lr;
    uint32_t fn = ld32(opd);
    ctx->r[2] = ld32(opd + 4);
    ctx->r[1] = (sp - 0x200) & ~15ull;
    ctx->lr = 0;
    ppu_call(ctx, fn);
    ctx->r[1] = sp;
    ctx->r[2] = toc;
    ctx->lr = lr;
}

/* ---------------- HLE binding ---------------- */
static std::unordered_map<std::string, hle_fn> &registry() {
    static std::unordered_map<std::string, hle_fn> m;
    return m;
}
static std::vector<hle_fn> g_hle;
static std::vector<std::atomic<int>> *g_hle_calls;

void hle_register(const char *name, hle_fn fn) { registry()[name] = fn; }

void hle_install_imports() {
    build_table();
    g_hle.assign(g_import_count, nullptr);
    g_hle_calls = new std::vector<std::atomic<int>>(g_import_count);
    int bound = 0;
    for (uint32_t i = 0; i < g_import_count; i++) {
        const ImportEntry &im = g_imports[i];
        auto it = registry().find(im.name);
        if (it != registry().end()) { g_hle[i] = it->second; bound++; }
        uint32_t opd = mem::HLE_OPD + i * 8;
        st32(opd, mem::HLE_CODE + i * 4);
        st32(opd + 4, 0);
        st32(im.stub_addr, opd);
    }
    LOG("hle", "%d/%u imports have implementations; the rest are logged stubs returning 0", bound, g_import_count);
}

// Runtime-internal functions callable as guest code (callbacks, thread
// entries). They live after the import slots.
static std::vector<hle_fn> g_internal;
static constexpr uint32_t INTERNAL_SLOT0 = 0x4000;

uint32_t hle_make_opd(hle_fn fn) {
    uint32_t slot = INTERNAL_SLOT0 + (uint32_t)g_internal.size();
    g_internal.push_back(fn);
    uint32_t opd = mem::HLE_OPD + slot * 8;
    st32(opd, mem::HLE_CODE + slot * 4);
    st32(opd + 4, 0);
    return opd;
}

void hle_dump_counts() {
    std::vector<std::pair<int, uint32_t>> v;
    for (uint32_t i = 0; i < g_import_count; i++)
        if (int n = (*g_hle_calls)[i]) v.push_back({n, i});
    std::sort(v.rbegin(), v.rend());
    for (size_t k = 0; k < v.size() && k < 15; k++)
        LOG("watchdog", "  %8d  %s%s", v[k].first, g_imports[v[k].second].name, g_hle[v[k].second] ? "" : " (stub)");
}

bool hle_dispatch(PPUContext *ctx, uint32_t addr) {
    if (addr >= mem::HLE_CODE + INTERNAL_SLOT0 * 4 && addr < mem::HLE_CODE + (INTERNAL_SLOT0 + g_internal.size()) * 4) {
        g_internal[(addr - mem::HLE_CODE) / 4 - INTERNAL_SLOT0](ctx);
        return true;
    }
    if (addr < mem::HLE_CODE || addr >= mem::HLE_CODE + g_import_count * 4) return false;
    uint32_t i = (addr - mem::HLE_CODE) / 4;
    const ImportEntry &im = g_imports[i];
    int n = (*g_hle_calls)[i]++;
    if (g_hle[i]) {
        if (getenv("KZ2_TRACE_HLE") && n < 20)
            LOG("hle", "%s(%llx, %llx, %llx, %llx)", im.name, ARG(0), ARG(1), ARG(2), ARG(3));
        thread::note_call(im.name, ctx, true);
        g_hle[i](ctx);
        thread::note_call(im.name, ctx, false);
    } else {
        thread::note_call(im.name, ctx, true);
        thread::note_call(im.name, ctx, false);
        if (n < 3)
            LOG("stub", "%s::%s(%llx, %llx, %llx, %llx) -> 0", im.module, im.name[0] ? im.name : "?", ARG(0), ARG(1), ARG(2), ARG(3));
        RET(0);
    }
    return true;
}
