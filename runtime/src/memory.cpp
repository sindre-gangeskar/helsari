// Guest address space: one 4 GiB reservation, committed lazily.
//
// Any access to an uncommitted page faults into a vectored exception
// handler that commits it (zero-filled) and resumes. This mirrors the PS3,
// where the game's regions are always backed, and keeps us from having to
// pre-commit the RSX/mmapper ranges.
#include "runtime.h"

#include <unordered_map>
#include <windows.h>

#include <map>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

uint8_t *g_mem;
uint32_t g_line_locks[LINE_LOCKS];
uint32_t g_line_seq[LINE_LOCKS];

namespace mem {
namespace {
constexpr uint64_t RESERVE = 0x100000000ull + 0x10000; // +64K so 16-byte accesses at the top don't escape
std::mutex g_lock;
std::map<uint32_t, uint32_t> g_free;   // user heap: addr -> size
std::map<uint32_t, uint32_t> g_used;
uint32_t g_stack_next = STACK_BASE;
uint32_t g_mmap_next = MMAP_BASE;
int g_fault_logs;

// "module+offset" for each of the first few host frames of a fault context,
// so a fault inside a system DLL (memcpy) still names its caller in kz2.exe.
std::string host_backtrace(const CONTEXT *ctx) {
    CONTEXT c = *ctx;
    std::string out;
    for (int i = 0; i < 6 && c.Rip; i++) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?", buf[MAX_PATH + 32];
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)c.Rip, &mod)) {
            GetModuleFileNameA(mod, name, sizeof name);
        }
        const char *base = strrchr(name, '\\');
        snprintf(buf, sizeof buf, " %s+0x%llx", base ? base + 1 : name, (unsigned long long)(c.Rip - (uintptr_t)mod));
        out += buf;
        DWORD64 image = 0;
        auto *fn = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
        if (!fn) {                                                   // leaf function: return address on top of stack
            c.Rip = *(DWORD64 *)c.Rsp;
            c.Rsp += 8;
            continue;
        }
        void *handler_data;
        DWORD64 frame;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fn, &c, &handler_data, &frame, nullptr);
    }
    return out;
}

// KZ2_WATCH_WRITE="ea:len,..." finds who writes guest memory: the host pages
// are made read-only once committed, a write faults here and is logged with a
// host backtrace (generated f_XXXXXXXX functions or runtime code), then the
// page is made writable and re-armed shortly after by watch_thread.
// Fixed tables so watch_add can extend them while the fault handler and
// watch_thread read them: an entry is filled in before the count covers it.
struct WatchPage { uintptr_t page; std::atomic<uint64_t> rearm_at{0}; };
constexpr int MAX_WATCH_PAGES = 1024, MAX_WATCH_RANGES = 64;
WatchPage g_watch_pages[MAX_WATCH_PAGES];
std::atomic<int> g_watch_count{0};
std::pair<uint32_t, uint32_t> g_watch_ranges[MAX_WATCH_RANGES];   // exact ranges; other writes to the pages pass silently
std::atomic<int> g_watch_range_count{0};
std::mutex g_watch_add_lock;

struct WatchList {
    WatchPage *begin() const { return g_watch_pages; }
    WatchPage *end() const { return g_watch_pages + g_watch_count.load(); }
    bool empty() const { return g_watch_count.load() == 0; }
} g_watch;

bool in_watch_range(uint32_t ea, uint32_t size) {
    for (int i = 0, n = g_watch_range_count.load(); i < n; i++) {
        auto [a, len] = g_watch_ranges[i];
        if (ea < a + len && a < ea + size) return true;
    }
    return false;
}
std::atomic<int> g_watch_logs{0};

uint64_t now_ms() { return GetTickCount64(); }

void watch_thread() {
    for (;;) {
        for (auto &w : g_watch) {
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery((void *)w.page, &mi, sizeof mi) || mi.State != MEM_COMMIT) continue;
            if (mi.Protect == PAGE_READWRITE && now_ms() >= w.rearm_at) {
                DWORD old;
                VirtualProtect((void *)w.page, 0x1000, PAGE_READONLY, &old);
            }
        }
        Sleep(1);
    }
}

thread_local uintptr_t t_step_page;   // watched page unprotected for one single-stepped store
thread_local uint32_t t_step_logged;  // in-range address of that store, to log the value it wrote
thread_local std::string t_step_report;

bool watch_fault(EXCEPTION_POINTERS *ep, uint8_t *addr) {
    uintptr_t page = (uintptr_t)addr & ~(uintptr_t)0xfff;
    for (auto &w : g_watch) {
        if (w.page != page) continue;
        if (ep->ExceptionRecord->ExceptionInformation[0] != 1) return false;   // reads never fault here
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)page, &mi, sizeof mi) || mi.State != MEM_COMMIT) {
            // First touch: commit the 64 KiB block and arm its watched pages now, so
            // this very write faults again and is logged (a fast fill would otherwise
            // complete before watch_thread arms the page).
            uintptr_t block = page & ~(uintptr_t)0xffff;
            if (!VirtualAlloc((void *)block, 0x10000, MEM_COMMIT, PAGE_READWRITE)) return false;
            for (auto &o : g_watch)
                if ((o.page & ~(uintptr_t)0xffff) == block) {
                    DWORD old;
                    VirtualProtect((void *)o.page, 0x1000, PAGE_READONLY, &old);
                }
            return true;
        }
        uint32_t ea = (uint32_t)(addr - g_mem);
        if (in_watch_range(ea, 16) && g_watch_logs < 300) {
            // The report is logged after the store (on_fault, single step), and only if
            // the word written matches KZ2_WATCH_VALUE when that is set.
            t_step_logged = ea & ~3u;
            PPUContext *ctx = thread::current();
            char line[512];
            snprintf(line, sizeof line, "watched write to 0x%x (guest lr 0x%llx, host:%s)\n", ea,
                     ctx ? (unsigned long long)ctx->lr : 0ull, host_backtrace(ep->ContextRecord).c_str());
            t_step_report = line;
            if (ctx) {   // in a memcpy: r3 dst, r4 source cursor, r5 length (backward copies start at the end)
                auto hex = [](uint32_t ea) {
                    std::string h;
                    for (int k = 0; k < 32; k += 4) {
                        uint32_t v;
                        memcpy(&v, g_mem + ea + k, 4);
                        char b[12];
                        snprintf(b, sizeof b, " %08x", __builtin_bswap32(v));
                        h += b;
                    }
                    return h;
                };
                uint32_t cur = (uint32_t)ctx->r[4], n = (uint32_t)ctx->r[5];
                snprintf(line, sizeof line, "  r3=0x%x r4=0x%x r5=0x%x; before r4:%s; at r4-r5:%s\n", (uint32_t)ctx->r[3], cur,
                         n, hex(cur - 32).c_str(), hex(cur - n).c_str());
                t_step_report += line;
                // the caller's saved state: non-volatile registers and what they point at
                for (int r = 26; r < 32; r++) {
                    uint32_t v = (uint32_t)ctx->r[r];
                    bool ptr = v >= 0x10000 && v < 0xf0000000;
                    snprintf(line, sizeof line, "  r%d=0x%x%s%s\n", r, v, ptr ? ":" : "", ptr ? hex(v & ~3u).c_str() : "");
                    t_step_report += line;
                }
                if (const char *e = getenv("KZ2_WATCH_PEEK")) {   // "reg:off,..." dumps [r<reg>+off]
                    for (const char *s = e; *s;) {
                        char *end;
                        int r = (int)strtol(s, &end, 10);
                        uint32_t off = *end == ':' ? strtoul(end + 1, &end, 16) : 0;
                        if (r >= 0 && r < 32) {
                            snprintf(line, sizeof line, "  [r%d+0x%x]:%s\n", r, off, hex((uint32_t)ctx->r[r] + off).c_str());
                            t_step_report += line;
                        }
                        s = *end == ',' ? end + 1 : end;
                    }
                }
            }
        }
        // Let this one store through, then single-step and re-protect (on_fault),
        // so every store to the page is seen, not just the first of a burst.
        DWORD old;
        VirtualProtect((void *)page, 0x1000, PAGE_READWRITE, &old);
        w.rearm_at = now_ms() + 1000;
        t_step_page = page;
        ep->ContextRecord->EFlags |= 0x100;   // trap flag
        return true;
    }
    return false;
}

} // namespace

// ---- write tracking of RSX local memory (see runtime.h) ----
namespace {
constexpr uint32_t TRACK_PAGES = LOCAL_SIZE >> 12;
std::atomic<uint8_t> g_armed[TRACK_PAGES];   // 1: read-only, not written since armed
const bool g_track_on = [] { const char *e = getenv("KZ2_TEX_TRACK"); return !e || atoi(e) != 0; }();
bool track_range(uint32_t ea, uint32_t size, uint32_t &p0, uint32_t &p1) {
    if (!size || ea < LOCAL_BASE || ea - LOCAL_BASE >= LOCAL_SIZE) return false;
    uint64_t end = std::min<uint64_t>((uint64_t)ea + size, (uint64_t)LOCAL_BASE + LOCAL_SIZE);
    p0 = (ea - LOCAL_BASE) >> 12, p1 = (uint32_t)((end - 1 - LOCAL_BASE) >> 12);
    return true;
}
void protect_pages(uint32_t p0, uint32_t p1, DWORD prot) {
    DWORD old;
    VirtualProtect(g_mem + LOCAL_BASE + ((uintptr_t)p0 << 12), (uintptr_t)(p1 - p0 + 1) << 12, prot, &old);
}
// A guest write to an armed page: unprotect it, mark it written.
bool track_fault(EXCEPTION_POINTERS *ep, uint8_t *addr) {
    if (ep->ExceptionRecord->ExceptionInformation[0] != 1) return false;
    uint64_t ea = (uint64_t)(addr - g_mem);
    if (ea < LOCAL_BASE || ea - LOCAL_BASE >= LOCAL_SIZE) return false;
    uint32_t p = (uint32_t)((ea - LOCAL_BASE) >> 12);
    if (!g_armed[p].exchange(0)) {
        // Another thread unprotected it meanwhile, or it was never ours: if the page
        // is writable now, retry the store; otherwise leave it to the other handlers.
        MEMORY_BASIC_INFORMATION mi;
        return VirtualQuery(addr, &mi, sizeof mi) && mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE;
    }
    protect_pages(p, p, PAGE_READWRITE);
    return true;
}
} // namespace

bool track_enabled() { return g_track_on; }

void track_arm(uint32_t ea, uint32_t size) {
    uint32_t p0, p1;
    if (!g_track_on || !track_range(ea, size, p0, p1)) return;
    // Arm runs of not-yet-armed, committed pages with one call each.
    for (uint32_t p = p0; p <= p1;) {
        if (g_armed[p].load()) { p++; continue; }
        uint32_t q = p;
        while (q + 1 <= p1 && !g_armed[q + 1].load()) q++;
        MEMORY_BASIC_INFORMATION mi;
        if (VirtualQuery(g_mem + LOCAL_BASE + ((uintptr_t)p << 12), &mi, sizeof mi) && mi.State == MEM_COMMIT) {
            for (uint32_t k = p; k <= q; k++) g_armed[k] = 1;
            protect_pages(p, q, PAGE_READONLY);
        }
        p = q + 1;
    }
}

bool track_written(uint32_t ea, uint32_t size) {
    uint32_t p0, p1;
    if (!g_track_on || !track_range(ea, size, p0, p1)) return true;
    for (uint32_t p = p0; p <= p1; p++)
        if (!g_armed[p].load(std::memory_order_relaxed)) return true;
    return false;
}

void track_unarm(uint32_t ea, uint32_t size) {
    uint32_t p0, p1;
    if (!g_track_on || !track_range(ea, size, p0, p1)) return;
    bool any = false;
    for (uint32_t p = p0; p <= p1; p++) any |= g_armed[p].exchange(0) != 0;
    if (any) protect_pages(p0, p1, PAGE_READWRITE);
}

bool watch_host_write(uint32_t ea, uint32_t size, const char *what) {
    track_unarm(ea, size);
    bool hit = false;
    for (auto &w : g_watch) {
        uintptr_t lo = (uintptr_t)(g_mem + ea), hi = lo + size;
        if (w.page + 0x1000 <= lo || w.page >= hi) continue;
        if (!hit && in_watch_range(ea, size) && g_watch_logs++ < 300)
            LOG("mem", "watched range written by host I/O: 0x%x+0x%x (%s)", ea, size, what);
        hit = true;
        DWORD old;
        w.rearm_at = now_ms() + 5000;
        VirtualProtect((void *)w.page, 0x1000, PAGE_READWRITE, &old);
    }
    return hit;
}

namespace {

LONG CALLBACK on_fault(EXCEPTION_POINTERS *ep) {
    auto *er = ep->ExceptionRecord;
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && t_step_page) {   // the watched store is done: re-arm
        DWORD old;
        VirtualProtect((void *)t_step_page, 0x1000, PAGE_READONLY, &old);
        t_step_page = 0;
        if (t_step_logged) {
            uint32_t v;
            memcpy(&v, g_mem + t_step_logged, 4);
            v = __builtin_bswap32(v);
            static const char *want = getenv("KZ2_WATCH_VALUE");
            if ((!want || v == strtoul(want, nullptr, 16)) && g_watch_logs++ < 300)
                LOG("mem", "%s  -> 0x%x now 0x%08x", t_step_report.c_str(), t_step_logged, v);
            t_step_logged = 0;
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    auto addr = (uint8_t *)er->ExceptionInformation[1];
    if (addr < g_mem || addr >= g_mem + RESERVE) return EXCEPTION_CONTINUE_SEARCH;
    if (g_track_on && track_fault(ep, addr)) return EXCEPTION_CONTINUE_EXECUTION;
    if (!g_watch.empty() && watch_fault(ep, addr)) return EXCEPTION_CONTINUE_EXECUTION;
    uint64_t ea = (uint64_t)(addr - g_mem);
    // A write to the NULL page is how the game crashes on purpose after a
    // fatal error (it stores 0x0deadca7 to address 0); on hardware it faults.
    if (ea < ELF_BASE && er->ExceptionInformation[0] == 1) guest_null_write((uint32_t)ea);
    if (ea < ELF_BASE && g_fault_logs < 20) {
        g_fault_logs++;
        LOG("mem", "guest NULL-page access at 0x%llx (host:%s) - committing zero page", ea,
            host_backtrace(ep->ContextRecord).c_str());
    }
    void *page = (void *)((uintptr_t)addr & ~(uintptr_t)0xffff);
    if (!VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}
} // namespace

uint64_t committed() {
    uint64_t n = 0;
    MEMORY_BASIC_INFORMATION mi;
    for (uint8_t *p = g_mem; p < g_mem + RESERVE && VirtualQuery(p, &mi, sizeof mi); p = (uint8_t *)mi.BaseAddress + mi.RegionSize)
        if (mi.State == MEM_COMMIT) n += mi.RegionSize;
    return n;
}

// Leak hunting: private committed memory outside the guest reservation,
// grouped by allocation (region) size. Driver buffers are few large blocks;
// heap growth is many small ones.
std::string host_regions() {
    static const uint64_t edges[] = {1 << 20, 16 << 20, 64 << 20};
    uint64_t bytes[4] = {}, count[4] = {};
    std::unordered_map<void *, uint64_t> alloc;     // allocation base -> committed bytes
    MEMORY_BASIC_INFORMATION mi;
    for (uint8_t *p = nullptr; VirtualQuery(p, &mi, sizeof mi); p = (uint8_t *)mi.BaseAddress + mi.RegionSize) {
        uint8_t *b = (uint8_t *)mi.BaseAddress;
        if (mi.State == MEM_COMMIT && mi.Type == MEM_PRIVATE && !(b >= g_mem && b < g_mem + RESERVE))
            alloc[mi.AllocationBase] += mi.RegionSize;
        if ((uint8_t *)mi.BaseAddress + mi.RegionSize < p) break;
    }
    for (auto &[base, n] : alloc) {
        int k = 0;
        while (k < 3 && n >= edges[k]) k++;
        bytes[k] += n, count[k]++;
    }
    char s[200];
    snprintf(s, sizeof s, "host blocks <1MB %llu (%llu MB), 1-16MB %llu (%llu MB), 16-64MB %llu (%llu MB), >=64MB %llu (%llu MB)",
             (unsigned long long)count[0], (unsigned long long)(bytes[0] >> 20), (unsigned long long)count[1],
             (unsigned long long)(bytes[1] >> 20), (unsigned long long)count[2], (unsigned long long)(bytes[2] >> 20),
             (unsigned long long)count[3], (unsigned long long)(bytes[3] >> 20));
    return s;
}

void init() {
    g_mem = (uint8_t *)VirtualAlloc(nullptr, RESERVE, MEM_RESERVE, PAGE_READWRITE);
    if (!g_mem) fatal("could not reserve 4 GiB of address space for guest memory");
    AddVectoredExceptionHandler(1, on_fault);
    g_free[USER_BASE] = USER_SIZE;
    if (const char *p = getenv("KZ2_WATCH_WRITE")) {
        for (const char *s = p; *s;) {
            char *end;
            uint32_t a = strtoul(s, &end, 16), n = *end == ':' ? strtoul(end + 1, &end, 16) : 4;
            watch_add(a, n);
            s = *end == ',' ? end + 1 : end;
        }
    }
}

// Starts logging writes to [ea, ea+size) from now on (see KZ2_WATCH_WRITE).
void watch_add(uint32_t ea, uint32_t size) {
    std::lock_guard<std::mutex> lk(g_watch_add_lock);
    static std::once_flag started;
    std::call_once(started, [] { std::thread(watch_thread).detach(); });
    int r = g_watch_range_count.load();
    if (r == MAX_WATCH_RANGES) return;
    g_watch_ranges[r] = {ea, size};
    g_watch_range_count = r + 1;
    for (uint32_t pg = ea & ~0xfffu; pg < ea + size; pg += 0x1000) {
        uintptr_t page = (uintptr_t)(g_mem + pg);
        bool have = false;
        for (auto &w : g_watch) have |= w.page == page;
        int n = g_watch_count.load();
        if (have || n == MAX_WATCH_PAGES) continue;
        g_watch_pages[n].page = page;
        g_watch_pages[n].rearm_at = 0;
        g_watch_count = n + 1;
    }
    LOG("mem", "watching writes to 0x%x+0x%x (%d pages watched)", ea, size, g_watch_count.load());
}

uint32_t alloc(uint32_t size, uint32_t align) {
    std::lock_guard<std::mutex> lk(g_lock);
    size = (size + 15) & ~15u;
    if (align < 16) align = 16;
    for (auto it = g_free.begin(); it != g_free.end(); ++it) {
        uint32_t a = (it->first + align - 1) & ~(align - 1);
        uint32_t pad = a - it->first;
        if (it->second < pad + size) continue;
        uint32_t base = it->first, total = it->second;
        g_free.erase(it);
        if (pad) g_free[base] = pad;
        if (total > pad + size) g_free[a + size] = total - pad - size;
        g_used[a] = size;
        return a;
    }
    LOG("mem", "user heap exhausted (request 0x%x)", size);
    return 0;
}

void free(uint32_t addr) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto u = g_used.find(addr);
    if (u == g_used.end()) return;
    uint32_t size = u->second;
    g_used.erase(u);
    auto it = g_free.emplace(addr, size).first;
    auto nx = std::next(it);
    if (nx != g_free.end() && it->first + it->second == nx->first) { it->second += nx->second; g_free.erase(nx); }
    if (it != g_free.begin()) {
        auto pv = std::prev(it);
        if (pv->first + pv->second == it->first) { pv->second += it->second; g_free.erase(it); }
    }
}

uint32_t alloc_stack(uint32_t size) {
    std::lock_guard<std::mutex> lk(g_lock);
    size = (size + 0xffff) & ~0xffffu;
    uint32_t a = g_stack_next;
    g_stack_next += size + 0x10000; // guard gap
    if (g_stack_next > STACK_END) fatal("out of guest stack space");
    return a;
}

uint32_t mmap_reserve(uint32_t size, uint32_t align) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (align < 0x10000) align = 0x10000;
    uint32_t a = (g_mmap_next + align - 1) & ~(align - 1);
    if ((uint64_t)a + size > MMAP_END) return 0;
    g_mmap_next = a + size;
    return a;
}

void ensure(uint32_t ea, uint32_t size) {
    // Kernel-mode accesses (ReadFile into guest memory) don't go through the
    // fault handler, so host I/O must commit its target range first.
    if (!size) return;
    uintptr_t lo = (uintptr_t)(g_mem + ea) & ~(uintptr_t)0xffff;
    uintptr_t hi = (uintptr_t)(g_mem + ea + (uint64_t)size);
    VirtualAlloc((void *)lo, hi - lo, MEM_COMMIT, PAGE_READWRITE);
}

std::string cstr(uint32_t ea) {
    if (!ea) return {};
    const char *p = (const char *)ptr(ea);
    return std::string(p, strnlen(p, 4096));
}

void write_cstr(uint32_t ea, const std::string &s, size_t max) {
    size_t n = s.size() < max - 1 ? s.size() : max - 1;
    memcpy(ptr(ea), s.data(), n);
    g_mem[ea + n] = 0;
}
} // namespace mem
