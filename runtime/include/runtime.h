/* Runtime-internal API (C++ side). Generated tables use the C-compatible
 * structs at the top. */
#pragma once
#include "ppu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FuncEntry { uint32_t addr; ppu_fn fn; } FuncEntry;
typedef struct ImportEntry { const char *module, *name; uint32_t nid, stub_addr; } ImportEntry;

extern const uint32_t g_func_count;
extern const FuncEntry g_funcs[];
extern const uint32_t g_import_count;
extern const ImportEntry g_imports[];

#ifdef __cplusplus
}

#include <cstdio>
#include <string>

/* ---- guest memory map ---- */
namespace mem {
constexpr uint32_t ELF_BASE = 0x00010000;
constexpr uint32_t HLE_OPD = 0x2ff00000;    /* fake descriptors for imports  */
constexpr uint32_t HLE_CODE = 0x2ff80000;   /* fake code addresses (4 bytes each) */
constexpr uint32_t USER_BASE = 0x30000000, USER_SIZE = 0x10000000;   /* sys_memory / heaps */
constexpr uint32_t RSX_IO = 0x40000000;     /* RSX driver structures (control, labels) */
constexpr uint32_t MMAP_BASE = 0x50000000, MMAP_END = 0xb0000000;    /* sys_mmapper */
constexpr uint32_t LOCAL_BASE = 0xc0000000, LOCAL_SIZE = 0x0f900000; /* RSX local memory */
constexpr uint32_t STACK_BASE = 0xd0000000, STACK_END = 0xe0000000;

void init();
uint64_t committed();                                 /* bytes of guest memory backed by host memory */
std::string host_regions();                           /* committed private host memory outside it, by block size */
uint32_t alloc(uint32_t size, uint32_t align = 16);  /* user region */
void free(uint32_t addr);
void ensure(uint32_t ea, uint32_t size);  /* commit before host (kernel) I/O touches it */
/* HELSARI_WATCH_WRITE: kernel I/O into watched memory doesn't fault, so host I/O
   reports itself here (and the pages are unprotected for it). True if watched. */
bool watch_host_write(uint32_t ea, uint32_t size, const char *what);
/* Write tracking of RSX local memory (texture change detection, memory.cpp):
   arm = make the pages read-only; a guest write faults once and marks the page
   written. written() is true if any page of the range was written (or never
   armed) since it was armed. Bulk host writers call unarm() first. */
void track_arm(uint32_t ea, uint32_t size);
bool track_written(uint32_t ea, uint32_t size);
void track_unarm(uint32_t ea, uint32_t size);
bool track_enabled();
void watch_add(uint32_t ea, uint32_t size);
/* HELSARI_HOOK_WATCH="ea:len": does [ea, ea+size) overlap it? (debug_hooks.cpp) */
bool hook_watched(uint32_t ea, uint32_t size);
uint32_t alloc_stack(uint32_t size);
uint32_t mmap_reserve(uint32_t size, uint32_t align);
inline void *ptr(uint32_t ea) { return g_mem + ea; }
std::string cstr(uint32_t ea);
void write_cstr(uint32_t ea, const std::string &s, size_t max);

/* Host code updating guest data that SPUs also update with lock-line atomics
   must hold the line's lock across the read-modify-write; a PUTLLC commit
   would otherwise overwrite the update (see line_lock in ppu.h). */
struct LineGuard {
    uint32_t ea;
    explicit LineGuard(uint32_t a) : ea(a) { line_lock(a); }
    ~LineGuard() { line_commit(ea); line_unlock(ea); }
    LineGuard(const LineGuard &) = delete;
    LineGuard &operator=(const LineGuard &) = delete;
};
}

/* Report a guest write to the NULL page (the game's deliberate crash after a
   fatal error) with backtrace and the error message, then exit. */
[[noreturn]] void guest_null_write(uint32_t ea);

/* ---- logging ---- */
void log_msg(const char *chan, const char *fmt, ...);
#define LOG(chan, ...) log_msg(chan, __VA_ARGS__)
[[noreturn]] void fatal(const char *fmt, ...);
/* Name the calling host thread (shown by the profiler and debuggers). */
void name_host_thread(const std::string &name);
/* Sleep about `us` microseconds without occupying the core (high-resolution
   waitable timer; plain Sleep() rounds up to a whole timer tick). */
void host_sleep_us(uint32_t us);
/* host_backoff (wait-loop step) and IdleBackoff (guest polling-loop detector). */
#include "idle_backoff.h"

/* ---- HLE ---- */
#define ARG(n) (ctx->r[3 + (n)])
#define ARG32(n) ((uint32_t)ctx->r[3 + (n)])
#define RET(v) (ctx->r[3] = (uint64_t)(int64_t)(v))
typedef void (*hle_fn)(PPUContext *ctx);
struct HleReg { const char *name; hle_fn fn; };
#define HLE(name) static void hle_##name(PPUContext *ctx)
void hle_register(const char *name, hle_fn fn);
void hle_install_imports();      /* fills the game's import stub slots */
bool hle_dispatch(PPUContext *ctx, uint32_t addr);
uint32_t hle_make_opd(hle_fn fn);  /* OPD for a runtime function, callable by guest code */

/* call a guest function descriptor (OPD) from host code, preserving r1/r2/lr */
void ppu_invoke(PPUContext *ctx, uint32_t opd);

/* ---- threads ---- */
void gcm_tl(const char *fmt, ...);
void gcm_post_job_mapped(const char *job);
void gcm_parked_wake(uint32_t ea, uint32_t size);   /* [ea, ea+size) was written: wake the RSX if parked on it */
uint32_t gcm_parked_ea();   /* guest address of the jump the RSX is parked on, 0 if none */
std::string tb_report(double frames);   /* HELSARI_TB_SITES: clock reads per site (core.cpp) */   /* frame timeline log around an F12 capture (hle_gcm.cpp) */
namespace thread {
PPUContext *create_context();
uint32_t current_id();
PPUContext *current();
uint64_t create(uint32_t entry_opd, uint64_t arg, int prio, uint32_t stack, const char *name, bool joinable);
void exit_current(uint64_t status) noexcept(false);
int join(uint64_t id, uint64_t *status);
uint32_t alloc_tls();
void set_tls_params(uint32_t img, uint32_t filesz, uint32_t memsz);
void run_main(uint32_t argv, uint32_t envp);
void note_call(const char *name, PPUContext *ctx, bool entering);  /* watchdog bookkeeping */
std::string call_report(const char *filter, double frames);         /* ms/frame per OS call (flip log) */
void add_wait(const char *name, uint64_t ns);                       /* waits outside OS calls, for call_report */
void dump_all();
}

/* ---- config ---- */
namespace cfg {
extern std::string dump_dir;   /* folder containing PS3_GAME */
extern std::string data_dir;   /* writable root for dev_hdd0 etc. */
extern std::string elf_path;
}

uint32_t syscall_dispatch(PPUContext *ctx, uint64_t n);
void hle_register_all();
#endif
