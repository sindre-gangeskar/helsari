// LV2 syscalls reached through the `sc` instruction (r11 = number).
#include "fs.h"
#include "kernel.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

using namespace kern;

namespace {
uint64_t usec_now() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
void put64(uint32_t ea, uint64_t v) { if (ea) st64(ea, v); }
void put32(uint32_t ea, uint32_t v) { if (ea) st32(ea, v); }

std::map<uint32_t, uint32_t> g_shm;   // mmapper memory id -> size
std::atomic<uint32_t> g_shm_next{0x08000001};
} // namespace

uint32_t syscall_dispatch(PPUContext *ctx, uint64_t n) {
    uint32_t id;
    int32_t r;
    switch (n) {
    /* ---- process ---- */
    case 1: return 1;                                    // sys_process_getpid
    case 3: case 22: case 26:                            // sys_process_exit variants
        LOG("sys", "process exit (%lld)", (long long)ARG(0));
        fflush(stdout);
        ExitProcess((UINT)ARG(0));
    case 25: put32(ARG32(1), 0x00240001); return OK;     // sys_process_get_sdk_version
    case 12: put32(ARG32(1), 0); return OK;              // get_number_of_object
    case 30: return EINVAL;                              // _sys_process_get_paramsfo

    /* ---- PPU threads ---- */
    case 41: thread::exit_current(ARG(0));               // _sys_ppu_thread_exit
    case 43: SwitchToThread(); return OK;                // yield
    case 44: {                                           // join(id, u64 *status)
        uint64_t st = 0;
        if (thread::join(ARG(0), &st)) return ESRCH;
        put64(ARG32(1), st);
        return OK;
    }
    case 45: return OK;                                  // detach
    case 47: return OK;                                  // set_priority
    case 48: put32(ARG32(1), 1001); return OK;           // get_priority
    case 49: put32(ARG32(0), (uint32_t)ctx->r[1] & ~0xffffu); put32(ARG32(0) + 4, 0x40000); return OK;
    case 56: return OK;                                  // rename

    /* ---- timers / time ---- */
    case 141: std::this_thread::sleep_for(std::chrono::microseconds(ARG(0))); return OK;
    case 142: std::this_thread::sleep_for(std::chrono::seconds(ARG(0))); return OK;
    case 144: put32(ARG32(0), 0); put32(ARG32(1), 0); return OK;
    case 145: {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        uint64_t t = (((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ull; // 100ns since 1970
        put64(ARG32(0), t / 10000000);
        put64(ARG32(1), (t % 10000000) * 100);
        return OK;
    }
    case 147: return 79800000;                           // timebase frequency

    /* ---- mutex / cond ---- */
    case 100: r = mutex_create(&id, ARG32(1) && (ld32(ARG32(1) + 4) == 0x10)); put32(ARG32(0), id); return r;
    case 101: return mutex_destroy(ARG32(0));
    case 102: return mutex_lock(ARG32(0), ARG(1));
    case 103: return mutex_trylock(ARG32(0));
    case 104: return mutex_unlock(ARG32(0));
    case 105: r = cond_create(&id, ARG32(1)); put32(ARG32(0), id); return r;
    case 106: return cond_destroy(ARG32(0));
    case 107: return cond_wait(ARG32(0), ARG(1));
    case 108: return cond_signal(ARG32(0), false);
    case 109: case 110: return cond_signal(ARG32(0), true);

    /* ---- semaphore ---- */
    case 90: r = sema_create(&id, (int32_t)ARG(2), (int32_t)ARG(3)); put32(ARG32(0), id); return r;
    case 91: return sema_destroy(ARG32(0));
    case 92: return sema_wait(ARG32(0), ARG(1));
    case 93: return sema_trywait(ARG32(0));
    case 94: return sema_post(ARG32(0), (int32_t)ARG(1));
    case 114: { int32_t v; r = sema_get_value(ARG32(0), &v); put32(ARG32(1), v); return r; }

    /* ---- event flag ---- */
    case 82: r = eflag_create(&id, ARG(2)); put32(ARG32(0), id); return r;
    case 83: return eflag_destroy(ARG32(0));
    case 85: { uint64_t res; r = eflag_wait(ARG32(0), ARG(1), ARG32(2), &res, ARG(4)); put64(ARG32(3), res); return r; }
    case 86: { uint64_t res; r = eflag_trywait(ARG32(0), ARG(1), ARG32(2), &res); put64(ARG32(3), res); return r; }
    case 87: return eflag_set(ARG32(0), ARG(1));
    case 118: return eflag_clear(ARG32(0), ARG(1));
    case 132: return OK;                                 // cancel
    case 139: { uint64_t v; r = eflag_get(ARG32(0), &v); put64(ARG32(1), v); return r; }

    /* ---- rwlock ---- */
    case 120: r = rwlock_create(&id); put32(ARG32(0), id); return r;
    case 121: return rwlock_destroy(ARG32(0));
    case 122: return rwlock_rlock(ARG32(0), ARG(1));
    case 123: return rwlock_tryrlock(ARG32(0));
    case 124: return rwlock_runlock(ARG32(0));
    case 125: return rwlock_wlock(ARG32(0), ARG(1));
    case 126: return rwlock_trywlock(ARG32(0));
    case 127: return rwlock_wunlock(ARG32(0));

    /* ---- event queue / port ---- */
    case 128: r = equeue_create(&id, ARG(2), (int)ARG(3)); put32(ARG32(0), id); return r;
    case 129: return equeue_destroy(ARG32(0));
    case 130: {                                          // receive: event comes back in r4-r7
        Event ev{};
        r = equeue_receive(ARG32(0), &ev, ARG(2));
        ctx->r[4] = ev.source, ctx->r[5] = ev.d1, ctx->r[6] = ev.d2, ctx->r[7] = ev.d3;
        return r;
    }
    case 131: {
        Event evs[64];
        int cnt = 0, max = (int)ARG(2) > 64 ? 64 : (int)ARG(2);
        r = equeue_tryreceive(ARG32(0), evs, max, &cnt);
        for (int i = 0; i < cnt; i++) {
            uint32_t e = ARG32(1) + 32 * i;
            st64(e, evs[i].source), st64(e + 8, evs[i].d1), st64(e + 16, evs[i].d2), st64(e + 24, evs[i].d3);
        }
        put32(ARG32(3), cnt);
        return r;
    }
    case 133: return equeue_drain(ARG32(0));
    case 134: r = eport_create(&id, (int)ARG(1), ARG(2)); put32(ARG32(0), id); return r;
    case 135: return eport_destroy(ARG32(0));
    case 136: return eport_connect(ARG32(0), ARG32(1));
    case 137: return eport_disconnect(ARG32(0));
    case 138: return eport_send(ARG32(0), ARG(1), ARG(2), ARG(3));

    /* ---- memory ---- */
    case 348: case 350: {                                // sys_memory_allocate[_from_container]
        uint32_t size = ARG32(0), flags = ARG32(n == 348 ? 1 : 2), out = ARG32(n == 348 ? 2 : 3);
        uint32_t a = mem::alloc(size, (flags & 0x400) ? 0x100000 : 0x10000);
        if (!a) return ENOMEM;
        put32(out, a);
        return OK;
    }
    case 349: mem::free(ARG32(0)); return OK;
    case 351: put64(ARG32(1), 0x40000ull); put64(ARG32(1) + 8, 0xf); put32(ARG32(1) + 16, 0x100000); return OK;
    case 352: put32(ARG32(0), 0x0d500000); put32(ARG32(0) + 4, 0x0a000000); return OK; // total/available
    case 324: put32(ARG32(0), 0x30000001); return OK;    // memory_container_create
    case 325: return OK;
    case 330: {                                          // mmapper_allocate_address(size, flags, align, *addr)
        uint32_t a = mem::mmap_reserve(ARG32(0), ARG32(2));
        if (!a) return ENOMEM;
        put32(ARG32(3), a);
        return OK;
    }
    case 331: return OK;
    case 332: case 333: {                                // allocate_shared_memory(key, size, flags, *id)
        uint32_t mid = g_shm_next++;
        g_shm[mid] = ARG32(1);
        put32(ARG32(3), mid);
        return OK;
    }
    case 334: return OK;                                 // map_shared_memory: memory is always backed
    case 335: put32(ARG32(1), 0); return OK;
    case 337: {                                          // search_and_map(start, id, flags, *addr)
        uint32_t size = g_shm.count(ARG32(1)) ? g_shm[ARG32(1)] : 0x100000;
        uint32_t a = mem::mmap_reserve(size, 0x100000);
        put32(ARG32(3), a);
        return a ? OK : ENOMEM;
    }
    case 336: case 338: return OK;

    /* ---- tty ---- */
    case 402: put32(ARG32(3), 0); return OK;
    case 403: {
        std::string s((const char *)mem::ptr(ARG32(1)), ARG32(2));
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        LOG("tty", "%s", s.c_str());
        put32(ARG32(3), ARG32(2));
        return OK;
    }

    /* ---- PRX loader: the game links everything statically or via imports ---- */
    case 480: case 481: case 482: case 483: case 494: case 496: case 497:
        LOG("sys", "PRX syscall %llu ignored", (unsigned long long)n);
        return ENOENT;

    /* ---- SPU: not emulated yet ---- */
    case 169: return OK;                                 // sys_spu_initialize

    /* ---- filesystem ---- */
    case 801: { uint32_t fd; r = fs::open(mem::cstr(ARG32(0)), (int32_t)ARG(1), &fd); if (!r) put32(ARG32(2), fd); return r; }
    case 802: { uint64_t got = 0; r = fs::read(ARG32(0), ARG32(1), ARG(2), &got); put64(ARG32(3), got); return r; }
    case 803: { uint64_t put = 0; r = fs::write(ARG32(0), ARG32(1), ARG(2), &put); put64(ARG32(3), put); return r; }
    case 804: return fs::close(ARG32(0));
    case 805: { uint32_t fd; r = fs::opendir(mem::cstr(ARG32(0)), &fd); if (!r) put32(ARG32(1), fd); return r; }
    case 806: { uint64_t nr = 0; r = fs::readdir(ARG32(0), ARG32(1), &nr); put64(ARG32(2), nr); return r; }
    case 807: return fs::closedir(ARG32(0));
    case 808: return fs::stat(mem::cstr(ARG32(0)), ARG32(1));
    case 809: return fs::fstat(ARG32(0), ARG32(1));
    case 811: return fs::mkdir(mem::cstr(ARG32(0)));
    case 812: return fs::rename(mem::cstr(ARG32(0)), mem::cstr(ARG32(1)));
    case 813: return fs::rmdir(mem::cstr(ARG32(0)));
    case 814: return fs::unlink(mem::cstr(ARG32(0)));
    case 818: { uint64_t pos = 0; r = fs::lseek(ARG32(0), (int64_t)ARG(1), (int)ARG(2), &pos); put64(ARG32(3), pos); return r; }
    }
    static std::atomic<int> logged[1024];
    if (n < 1024 && logged[n]++ < 3)
        LOG("sys", "unimplemented syscall %llu (%llx, %llx, %llx, %llx) -> 0", (unsigned long long)n, ARG(0), ARG(1), ARG(2), ARG(3));
    return OK;
}

extern "C" void ppu_syscall(PPUContext *ctx) {
    static const char *names[1024];
    static std::once_flag once;
    std::call_once(once, [] {
        for (int i = 0; i < 1024; i++) { char *s = new char[16]; snprintf(s, 16, "syscall_%d", i); names[i] = s; }
    });
    uint64_t n = ctx->r[11];
    const char *nm = n < 1024 ? names[n] : "syscall_?";
    thread::note_call(nm, ctx, true);
    ctx->r[3] = (uint64_t)(int64_t)(int32_t)syscall_dispatch(ctx, n);
    thread::note_call(nm, ctx, false);
}
