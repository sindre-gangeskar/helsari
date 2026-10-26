// Guest address space: one 4 GiB reservation, committed lazily.
//
// Any access to an uncommitted page faults into a vectored exception
// handler that commits it (zero-filled) and resumes. This mirrors the PS3,
// where the game's regions are always backed, and keeps us from having to
// pre-commit the RSX/mmapper ranges.
#include "runtime.h"

#include <windows.h>

#include <map>
#include <mutex>

uint8_t *g_mem;
uint32_t g_line_locks[LINE_LOCKS];

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

LONG CALLBACK on_fault(EXCEPTION_POINTERS *ep) {
    auto *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    auto addr = (uint8_t *)er->ExceptionInformation[1];
    if (addr < g_mem || addr >= g_mem + RESERVE) return EXCEPTION_CONTINUE_SEARCH;
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

void init() {
    g_mem = (uint8_t *)VirtualAlloc(nullptr, RESERVE, MEM_RESERVE, PAGE_READWRITE);
    if (!g_mem) fatal("could not reserve 4 GiB of address space for guest memory");
    AddVectoredExceptionHandler(1, on_fault);
    g_free[USER_BASE] = USER_SIZE;
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
