// Entry point: load the game's data segments from the user's decrypted
// EBOOT, bind HLE imports, start the PS3 main thread.
#include "runtime.h"

#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>

#include <atomic>
#include <filesystem>
#include <new>
#include <thread>
#include <vector>

namespace cfg {
std::string dump_dir = HELSARI_DEFAULT_DUMP;
std::string data_dir;
std::string elf_path = HELSARI_DEFAULT_ELF;
}

void register_sys();
void register_gcm();
void register_io();
void register_spurs();
void register_spurs_kernel();
void register_spurs_tasks();
void register_sync();
void register_savedata();
void start_profiler();
void hle_dump_counts();
void rsx_dump_methods();
void spurs_tasks_dump();

void hle_register_all() {
    register_sys();
    register_gcm();
    register_io();
    register_spurs();
    register_spurs_kernel();
    register_spurs_tasks();
    register_sync();
    register_savedata();
    start_profiler();
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static void load_elf(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) fatal("cannot open decrypted EBOOT '%s' (run the build, or pass --elf)", path.c_str());
    std::vector<uint8_t> d;
    fseek(f, 0, SEEK_END);
    d.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(d.data(), 1, d.size(), f);
    fclose(f);
    if (d.size() < 64 || memcmp(d.data(), "\x7f" "ELF", 4)) fatal("'%s' is not a decrypted ELF", path.c_str());
    uint64_t phoff = be64(&d[32]);
    uint16_t phes = be16(&d[54]), phn = be16(&d[56]);
    for (int i = 0; i < phn; i++) {
        const uint8_t *ph = &d[phoff + i * phes];
        uint32_t type = be32(ph);
        uint64_t off = be64(ph + 8), va = be64(ph + 16), fsz = be64(ph + 32), msz = be64(ph + 40);
        if (type != 1 || !msz) continue;
        memcpy(mem::ptr((uint32_t)va), &d[off], fsz);
        memset(mem::ptr((uint32_t)(va + fsz)), 0, msz - fsz);
        LOG("loader", "segment 0x%08llx-0x%08llx (file 0x%llx)", va, va + msz, fsz);
    }
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    auto *er = ep->ExceptionRecord;
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)er->ExceptionAddress, &mod);
    char name[MAX_PATH] = "?";
    if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
    LOG("CRASH", "exception 0x%08lx at %p (%s+0x%llx), access address 0x%llx", er->ExceptionCode, er->ExceptionAddress,
        strrchr(name, 92) ? strrchr(name, 92) + 1 : name, (unsigned long long)((uint8_t *)er->ExceptionAddress - (uint8_t *)mod),
        er->NumberParameters > 1 ? (unsigned long long)er->ExceptionInformation[1] : 0ull);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

void start_alloc_tracing();
extern "C" const char helsari_build_stamp[];   // build/helsari_build_stamp.cpp

int main(int argc, char **argv) {
    // One instance at a time: two would share the save data and fight over input.
    CreateMutexA(nullptr, FALSE, "Local\\helsari-instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "helsari: another instance is already running\n");
        MessageBoxA(nullptr, "Helsari 2 is already running.", "Helsari 2", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    // 1 ms timer resolution (the default 15.6 ms tick stretches every short sleep and timed wait).
    timeBeginPeriod(1);
    SetUnhandledExceptionFilter(on_crash);
    SetProcessDPIAware();  // render at real pixel size on scaled displays
    std::filesystem::path exe_dir = std::filesystem::path(argv[0]).parent_path();
    cfg::data_dir = (exe_dir / "data").string();
    for (int i = 1; i + 1 < argc; i++) {
        std::string a = argv[i];
        if (a == "--dump") cfg::dump_dir = argv[++i];
        else if (a == "--elf") cfg::elf_path = argv[++i];
        else if (a == "--data") cfg::data_dir = argv[++i];
    }
    for (auto *s : {&cfg::dump_dir, &cfg::data_dir})
        for (auto &c : *s) if (c == '\\') c = '/';
    if (!std::filesystem::exists(cfg::dump_dir + "/PS3_GAME/PARAM.SFO"))
        fatal("Game dump not found at '%s' (pass --dump <folder containing PS3_GAME>)", cfg::dump_dir.c_str());
    for (const char *d : {"/dev_hdd0/game", "/dev_hdd0/home/00000001/savedata", "/dev_hdd1"})
        std::filesystem::create_directories(cfg::data_dir + d);
    LOG("main", "build: %s %s, stamp %s", __DATE__, __TIME__, helsari_build_stamp);   // which build wrote this log
    start_alloc_tracing();
    LOG("main", "dump: %s", cfg::dump_dir.c_str());
    LOG("main", "data: %s", cfg::data_dir.c_str());

    mem::init();
    load_elf(cfg::elf_path);
    hle_register_all();
    hle_install_imports();

    // argv/envp for crt0: 64-bit pointer arrays
    uint32_t path = mem::alloc(64);
    mem::write_cstr(path, "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN", 64);
    uint32_t av = mem::alloc(16), ev = mem::alloc(16);
    st64(av, path);
    st64(av + 8, 0);
    st64(ev, 0);
    if (const char *w = getenv("HELSARI_WATCHDOG")) {
        int secs = atoi(w) > 0 ? atoi(w) : 10;
        std::thread([secs] {
            for (;;) {
                Sleep(secs * 1000);
                LOG("watchdog", "thread states:");
                thread::dump_all();
                LOG("watchdog", "most-called imports:");
                hle_dump_counts();
                rsx_dump_methods();
                spurs_tasks_dump();
            }
        }).detach();
    }
    thread::run_main(av, ev);
    LOG("main", "main thread returned");
    return 0;
}

// Leak hunting: every allocation of 8 MB or more - C++ new, and VirtualAlloc /
// HeapAlloc calls made by any loaded DLL (the graphics driver) - is logged with
// its host call stack ("module+offset", symbolize helsari2.exe offsets with llvm-nm), up to 60.
namespace {
std::atomic<int> g_big_allocs{0};
void log_big_alloc(const char *what, size_t n) {
    if (n < (8u << 20) || g_big_allocs++ >= 60) return;
    void *frames[12];
    USHORT k = RtlCaptureStackBackTrace(2, 12, frames, nullptr);
    char out[768];
    int len = snprintf(out, sizeof out, "%s of %zu MB from", what, n >> 20);
    for (USHORT i = 0; i < k && len < (int)sizeof out - 48; i++) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)frames[i], &mod))
            GetModuleFileNameA(mod, name, sizeof name);
        const char *base = strrchr(name, '\\');
        len += snprintf(out + len, sizeof out - len, " %s+0x%llx", base ? base + 1 : name,
                        (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)mod));
    }
    LOG("mem", "%s", out);
}

using VirtualAllocFn = LPVOID(WINAPI *)(LPVOID, SIZE_T, DWORD, DWORD);
using HeapAllocFn = LPVOID(WINAPI *)(HANDLE, DWORD, SIZE_T);
VirtualAllocFn g_real_virtual_alloc;
HeapAllocFn g_real_heap_alloc;
LPVOID WINAPI logged_virtual_alloc(LPVOID a, SIZE_T n, DWORD type, DWORD prot) {
    if (type & MEM_COMMIT) log_big_alloc("VirtualAlloc", n);
    return g_real_virtual_alloc(a, n, type, prot);
}
LPVOID WINAPI logged_heap_alloc(HANDLE h, DWORD flags, SIZE_T n) {
    log_big_alloc("HeapAlloc", n);
    return g_real_heap_alloc(h, flags, n);
}
// The native call under VirtualAlloc; some drivers call it directly.
using NtAllocFn = LONG(NTAPI *)(HANDLE, PVOID *, ULONG_PTR, PSIZE_T, ULONG, ULONG);
NtAllocFn g_real_nt_alloc;
LONG NTAPI logged_nt_alloc(HANDLE proc, PVOID *base, ULONG_PTR zero, PSIZE_T size, ULONG type, ULONG prot) {
    if ((type & MEM_COMMIT) && size) log_big_alloc("NtAllocateVirtualMemory", *size);
    return g_real_nt_alloc(proc, base, zero, size, type, prot);
}

// Point every loaded module's import of `real` (any of its addresses) at `hook`.
int patch_imports(void *const *reals, int nreal, void *hook) {
    HMODULE mods[1024];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &bytes)) return 0;
    int patched = 0;
    for (DWORD i = 0; i < bytes / sizeof(HMODULE); i++) {
        auto *base = (uint8_t *)mods[i];
        auto *dos = (IMAGE_DOS_HEADER *)base;
        auto *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) continue;
        for (auto *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++)
            for (auto *t = (IMAGE_THUNK_DATA *)(base + d->FirstThunk); t->u1.Function; t++)
                for (int r = 0; r < nreal; r++)
                    if ((void *)t->u1.Function == reals[r]) {
                        DWORD old;
                        VirtualProtect(&t->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
                        t->u1.Function = (ULONG_PTR)hook;
                        VirtualProtect(&t->u1.Function, sizeof(void *), old, &old);
                        patched++;
                    }
    }
    return patched;
}

// HELSARI_TRACE_BIG_ALLOCS=1: hook the allocators once the graphics driver is loaded.
void hook_allocators() {
    HMODULE k32 = GetModuleHandleA("kernel32.dll"), kb = GetModuleHandleA("kernelbase.dll");
    void *va[2] = {(void *)GetProcAddress(k32, "VirtualAlloc"), (void *)GetProcAddress(kb, "VirtualAlloc")};
    void *ha[3] = {(void *)GetProcAddress(k32, "HeapAlloc"), (void *)GetProcAddress(kb, "HeapAlloc"),
                   (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlAllocateHeap")};
    void *nt[1] = {(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtAllocateVirtualMemory")};
    g_real_virtual_alloc = (VirtualAllocFn)va[1];
    g_real_heap_alloc = (HeapAllocFn)ha[2];
    g_real_nt_alloc = (NtAllocFn)nt[0];
    int n = patch_imports(va, 2, (void *)logged_virtual_alloc) + patch_imports(ha, 3, (void *)logged_heap_alloc) +
            patch_imports(nt, 1, (void *)logged_nt_alloc);
    LOG("mem", "allocation tracing: patched %d imports", n);
}
} // namespace

void start_alloc_tracing() {
    if (!getenv("HELSARI_TRACE_BIG_ALLOCS")) return;
    std::thread([] { Sleep(15000); hook_allocators(); }).detach();
}

void *operator new(size_t n) {
    log_big_alloc("new", n);
    if (void *p = malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](size_t n) { return operator new(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }
