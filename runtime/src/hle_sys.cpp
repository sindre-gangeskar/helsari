// sysPrxForUser, sys_fs, cellSysmodule, cellGame, cellSysutil, cellRtc.
#include "fs.h"
#include "kernel.h"

#include <windows.h>

#include <chrono>
#include <map>
#include <mutex>

#define REG(n) hle_register(#n, hle_##n)

namespace {
uint64_t usec_now() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
void put32(uint32_t ea, uint32_t v) { if (ea) st32(ea, v); }
void put64(uint32_t ea, uint64_t v) { if (ea) st64(ea, v); }
} // namespace

/* =============================== sysPrxForUser =============================== */

HLE(sys_initialize_tls) {
    // (main_thread_id, tls_seg_addr, tls_seg_size, tls_mem_size)
    thread::set_tls_params(ARG32(1), ARG32(2), ARG32(3));
    ctx->r[13] = thread::alloc_tls() + 0x7030;
    LOG("sys", "sys_initialize_tls(img=0x%x file=0x%x mem=0x%x) r13=0x%llx", ARG32(1), ARG32(2), ARG32(3), ctx->r[13]);
    RET(0);
}

// sys_lwmutex_t: u32 owner, u32 waiter, u32 attribute, u32 recursive_count, u32 sleep_queue, u32 pad
// We keep the real lock in a kernel mutex whose id lives in sleep_queue.
HLE(sys_lwmutex_create) {
    uint32_t lw = ARG32(0), attr = ARG32(1);
    bool recursive = attr && ld32(attr + 4) == 0x10;
    uint32_t id;
    kern::mutex_create(&id, recursive);
    st32(lw + 0, 0xffffffff);
    st32(lw + 4, 0);
    st32(lw + 8, attr ? ld32(attr) | ld32(attr + 4) : 0);
    st32(lw + 12, 0);
    st32(lw + 16, id);
    st32(lw + 20, 0);
    RET(0);
}
HLE(sys_lwmutex_destroy) { RET(kern::mutex_destroy(ld32(ARG32(0) + 16))); }
HLE(sys_lwmutex_lock) {
    uint32_t lw = ARG32(0);
    int32_t r = kern::mutex_lock(ld32(lw + 16), ARG(1));
    if (!r) { st32(lw, thread::current_id()); st32(lw + 12, ld32(lw + 12) + 1); }
    RET(r);
}
HLE(sys_lwmutex_trylock) {
    uint32_t lw = ARG32(0);
    int32_t r = kern::mutex_trylock(ld32(lw + 16));
    if (!r) { st32(lw, thread::current_id()); st32(lw + 12, ld32(lw + 12) + 1); }
    RET(r);
}
HLE(sys_lwmutex_unlock) {
    uint32_t lw = ARG32(0);
    uint32_t c = ld32(lw + 12);
    int32_t r = kern::mutex_unlock(ld32(lw + 16));
    if (!r) { st32(lw + 12, c - 1); if (c <= 1) st32(lw, 0xffffffff); }
    RET(r);
}

// sys_lwcond_t: u32 lwmutex, u32 lwcond_queue (our kernel cond id)
HLE(sys_lwcond_create) {
    uint32_t lc = ARG32(0), lw = ARG32(1);
    uint32_t id;
    int32_t r = kern::cond_create(&id, ld32(lw + 16));
    st32(lc, lw);
    st32(lc + 4, id);
    RET(r);
}
HLE(sys_lwcond_destroy) { RET(kern::cond_destroy(ld32(ARG32(0) + 4))); }
HLE(sys_lwcond_wait) {
    uint32_t lc = ARG32(0), lw = ld32(lc);
    uint32_t saved = ld32(lw + 12);
    int32_t r = kern::cond_wait(ld32(lc + 4), ARG(1));
    st32(lw, thread::current_id());
    st32(lw + 12, saved);
    RET(r);
}
HLE(sys_lwcond_signal) { RET(kern::cond_signal(ld32(ARG32(0) + 4), false)); }
HLE(sys_lwcond_signal_all) { RET(kern::cond_signal(ld32(ARG32(0) + 4), true)); }

HLE(sys_ppu_thread_create) {
    // (u64 *thread_id, u32 entry_opd, u64 arg, s32 prio, u32 stacksize, u64 flags, const char *name)
    std::string name = mem::cstr(ARG32(6));
    uint64_t id = thread::create(ARG32(1), ARG(2), (int)ARG(3), ARG32(4), name.c_str(), ARG(5) & 1);
    put64(ARG32(0), id);
    RET(0);
}
HLE(sys_ppu_thread_get_id) { put64(ARG32(0), thread::current_id()); RET(0); }
HLE(sys_ppu_thread_exit) { thread::exit_current(ARG(0)); }
HLE(sys_time_get_system_time) { RET(usec_now()); }
HLE(sys_process_exit) {
    LOG("sys", "sys_process_exit(%d)", (int)ARG(0));
    fflush(stdout);
    ExitProcess((UINT)ARG(0));
}
HLE(sys_prx_exitspawn_with_level) {
    LOG("sys", "sys_prx_exitspawn_with_level: game asked to exit");
    fflush(stdout);
    ExitProcess(0);
}
HLE(sys_process_is_stack) { RET(ARG32(0) >= mem::STACK_BASE && ARG32(0) < mem::STACK_END); }
HLE(_sys_process_atexitspawn) { RET(0); }
HLE(_sys_process_at_Exitspawn) { RET(0); }
HLE(sys_prx_register_library) { RET(0); }

// sys heap: thin wrappers over the user heap
HLE(_sys_heap_create_heap) { static uint32_t next = 1; RET(next++); }
HLE(_sys_heap_delete_heap) { RET(0); }
HLE(_sys_heap_malloc) { RET(mem::alloc(ARG32(1), 16)); }
HLE(_sys_heap_memalign) { RET(mem::alloc(ARG32(2), ARG32(1))); }
HLE(_sys_heap_free) { mem::free(ARG32(1)); RET(0); }

// mmapper "memory" objects: address space is always backed, so just track ids
HLE(sys_mmapper_allocate_memory) {
    static uint32_t next = 0x09000001;
    put32(ARG32(2), next++);
    RET(0);
}
HLE(sys_mmapper_map_memory) { RET(0); }
HLE(sys_mmapper_unmap_memory) { put32(ARG32(1), 0); RET(0); }
HLE(sys_mmapper_free_memory) { RET(0); }

/* =================================== sys_fs ================================== */

HLE(cellFsOpen) {
    uint32_t fd = 0;
    int32_t r = fs::open(mem::cstr(ARG32(0)), (int32_t)ARG(1), &fd);
    if (!r) put32(ARG32(2), fd);
    RET(r);
}
HLE(cellFsRead) { uint64_t n = 0; int32_t r = fs::read(ARG32(0), ARG32(1), ARG(2), &n); put64(ARG32(3), n); RET(r); }
HLE(cellFsWrite) { uint64_t n = 0; int32_t r = fs::write(ARG32(0), ARG32(1), ARG(2), &n); put64(ARG32(3), n); RET(r); }
HLE(cellFsClose) { RET(fs::close(ARG32(0))); }
HLE(cellFsLseek) { uint64_t p = 0; int32_t r = fs::lseek(ARG32(0), (int64_t)ARG(1), (int)ARG(2), &p); put64(ARG32(3), p); RET(r); }
HLE(cellFsStat) {
    int32_t r = fs::stat(mem::cstr(ARG32(0)), ARG32(1));
    static int n;
    if (n++ < 10) LOG("fs", "stat '%s' -> %x mode=%o size=%llu", mem::cstr(ARG32(0)).c_str(), r, ld32(ARG32(1)), (unsigned long long)ld64(ARG32(1) + 36));
    RET(r);
}
HLE(cellFsFstat) { RET(fs::fstat(ARG32(0), ARG32(1))); }
HLE(cellFsOpendir) { uint32_t fd = 0; int32_t r = fs::opendir(mem::cstr(ARG32(0)), &fd); if (!r) put32(ARG32(1), fd); RET(r); }
HLE(cellFsReaddir) { uint64_t n = 0; int32_t r = fs::readdir(ARG32(0), ARG32(1), &n); put64(ARG32(2), n); RET(r); }
HLE(cellFsClosedir) { RET(fs::closedir(ARG32(0))); }
HLE(cellFsMkdir) { RET(fs::mkdir(mem::cstr(ARG32(0)))); }
HLE(cellFsRmdir) { RET(fs::rmdir(mem::cstr(ARG32(0)))); }
HLE(cellFsUnlink) { RET(fs::unlink(mem::cstr(ARG32(0)))); }
HLE(cellFsFsync) { RET(0); }
HLE(cellFsChmod) { RET(0); }
HLE(cellFsUtime) { RET(0); }

/* =============================== cellSysmodule =============================== */

HLE(cellSysmoduleInitialize) { RET(0); }
HLE(cellSysmoduleFinalize) { RET(0); }
HLE(cellSysmoduleSetMemcontainer) { RET(0); }
HLE(cellSysmoduleLoadModule) { LOG("sysmodule", "load module 0x%04x", ARG32(0)); RET(0); }
HLE(cellSysmoduleUnloadModule) { RET(0); }

/* ================================== cellGame ================================= */

HLE(cellGameBootCheck) {
    // (u32 *type, u32 *attributes, CellGameContentSize *size, char dirName[32])
    put32(ARG32(0), 1);  // CELL_GAME_GAMETYPE_DISC
    put32(ARG32(1), 0);
    if (ARG32(2)) { st32(ARG32(2), 0x7fffffff); st32(ARG32(2) + 4, 0); st32(ARG32(2) + 8, 0); }
    if (ARG32(3)) mem::write_cstr(ARG32(3), "", 32);
    RET(0);
}
HLE(cellGamePatchCheck) { RET(0x8002cb02); }  // CELL_GAME_ERROR_NOTPATCH-style: no patch installed
HLE(cellGameContentPermit) {
    mem::write_cstr(ARG32(0), "/dev_bdvd/PS3_GAME", 128);
    mem::write_cstr(ARG32(1), "/dev_bdvd/PS3_GAME/USRDIR", 128);
    RET(0);
}
HLE(cellGameGetParamInt) {
    // 50 PARENTAL_LEVEL, 60 RESOLUTION, 61 SOUND_FORMAT
    uint32_t v = ARG32(0) == 60 ? 0x3f : ARG32(0) == 61 ? 0x17 : ARG32(0) == 50 ? 5 : 0;
    put32(ARG32(1), v);
    RET(0);
}
HLE(cellGameGetParamString) {
    const char *s = ARG32(0) == 100 ? "Killzone 2" : ARG32(0) == 102 ? "BCES00081" : ARG32(0) == 104 ? "01.00" : "";
    mem::write_cstr(ARG32(1), s, ARG32(2) ? ARG32(2) : 1);
    RET(0);
}

/* ================================= cellSysutil ================================ */

namespace {
struct SysutilCb { uint32_t func, userdata; };
SysutilCb g_cbs[4];
} // namespace

HLE(cellSysutilRegisterCallback) {
    uint32_t slot = ARG32(0);
    if (slot < 4) g_cbs[slot] = {ARG32(1), ARG32(2)};
    RET(0);
}
HLE(cellSysutilUnregisterCallback) { if (ARG32(0) < 4) g_cbs[ARG32(0)] = {}; RET(0); }
HLE(cellSysutilCheckCallback) { RET(0); }  // no system events are generated yet
HLE(cellSysutilGetSystemParamInt) {
    uint32_t id = ARG32(0), v = 0;
    switch (id) {
    case 0x111: v = 1; break;  // language: English (US)
    case 0x112: v = 1; break;  // enter button: cross
    case 0x114: v = 0; break;  // date format
    case 0x115: v = 1; break;  // time format 24h
    case 0x116: v = 0; break;  // timezone
    case 0x117: v = 0; break;  // summer time
    case 0x121: v = 0; break;  // game parental level
    }
    put32(ARG32(1), v);
    RET(0);
}
HLE(cellSysutilGetSystemParamString) {
    mem::write_cstr(ARG32(1), ARG32(0) == 0x113 ? "Player" : "", ARG32(2) ? ARG32(2) : 1);
    RET(0);
}
HLE(cellSysutilEnableBgmPlayback) { RET(0); }
HLE(cellSysutilDisableBgmPlayback) { RET(0); }
HLE(cellSysCacheMount) {
    // CellSysCacheParam: char cacheId[32], char getCachePath[1024], u32 reserved
    uint32_t p = ARG32(0);
    mem::write_cstr(p + 32, "/dev_hdd1/caches", 1024);
    fs::mkdir("/dev_hdd1/caches");
    RET(0);
}

// video out: report a 1280x720 16:9 display
HLE(cellVideoOutGetState) {
    uint32_t s = ARG32(2);
    memset(mem::ptr(s), 0, 16);
    st8(s + 0, 0);      // CELL_VIDEO_OUT_OUTPUT_STATE_ENABLED
    st8(s + 1, 1);      // color space RGB
    st8(s + 8, 2);      // resolutionId 720p
    st8(s + 9, 1);      // progressive
    st8(s + 10, 0);
    st8(s + 11, 2);     // aspect 16:9
    st16(s + 14, 1);    // 59.94Hz
    RET(0);
}
HLE(cellVideoOutGetResolution) {
    static const uint16_t res[][2] = {{0, 0}, {1920, 1080}, {1280, 720}, {0, 0}, {720, 480}, {720, 576}};
    uint32_t id = ARG32(0);
    if (id >= 6 || !res[id][0]) { RET(0x8002b222); return; }
    st16(ARG32(1), res[id][0]);
    st16(ARG32(1) + 2, res[id][1]);
    RET(0);
}
HLE(cellVideoOutGetResolutionAvailability) { RET(ARG32(1) == 2 || ARG32(1) == 4); }
HLE(cellVideoOutConfigure) {
    LOG("video", "configure resolutionId=%u format=%u pitch=%u", ld8(ARG32(1)), ld8(ARG32(1) + 1), ld32(ARG32(1) + 8));
    RET(0);
}
HLE(cellVideoOutSetGamma) { RET(0); }
HLE(cellAudioOutGetSoundAvailability) { RET(ARG32(1) == 0 ? 2 : 0); }  // LPCM stereo
HLE(cellAudioOutGetNumberOfDevice) { RET(1); }
HLE(cellAudioOutGetDeviceInfo) { memset(mem::ptr(ARG32(2)), 0, 0x88); RET(0); }
HLE(cellAudioOutConfigure) { RET(0); }

/* =================================== cellRtc ================================== */

namespace {
uint64_t rtc_tick() {  // microseconds since 0001-01-01
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; // 100ns since 1601
    return t / 10 + 50491123200000000ull;
}
} // namespace
HLE(cellRtcGetCurrentTick) { st64(ARG32(0), rtc_tick()); RET(0); }
HLE(cellRtcGetCurrentClockLocalTime) {
    SYSTEMTIME s;
    GetLocalTime(&s);
    uint32_t p = ARG32(0); // CellRtcDateTime: u16 year, u16 month, u16 day, u16 hour, u16 minute, u16 second, u32 usec
    st16(p, s.wYear); st16(p + 2, s.wMonth); st16(p + 4, s.wDay);
    st16(p + 6, s.wHour); st16(p + 8, s.wMinute); st16(p + 10, s.wSecond); st32(p + 12, s.wMilliseconds * 1000);
    RET(0);
}
HLE(cellRtcGetDayOfWeek) {
    int y = (int)ARG(0), m = (int)ARG(1), d = (int)ARG(2);
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    RET((y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7);
}

void register_sys() {
    REG(sys_initialize_tls); REG(sys_lwmutex_create); REG(sys_lwmutex_destroy); REG(sys_lwmutex_lock);
    REG(sys_lwmutex_trylock); REG(sys_lwmutex_unlock); REG(sys_lwcond_create); REG(sys_lwcond_destroy);
    REG(sys_lwcond_wait); REG(sys_lwcond_signal); REG(sys_lwcond_signal_all); REG(sys_ppu_thread_create);
    REG(sys_ppu_thread_get_id); REG(sys_ppu_thread_exit); REG(sys_time_get_system_time); REG(sys_process_exit);
    REG(sys_prx_exitspawn_with_level); REG(sys_process_is_stack); REG(_sys_process_atexitspawn);
    REG(_sys_process_at_Exitspawn); REG(sys_prx_register_library); REG(_sys_heap_create_heap);
    REG(_sys_heap_delete_heap); REG(_sys_heap_malloc); REG(_sys_heap_memalign); REG(_sys_heap_free);
    REG(sys_mmapper_allocate_memory); REG(sys_mmapper_map_memory); REG(sys_mmapper_unmap_memory);
    REG(sys_mmapper_free_memory);
    REG(cellFsOpen); REG(cellFsRead); REG(cellFsWrite); REG(cellFsClose); REG(cellFsLseek); REG(cellFsStat);
    REG(cellFsFstat); REG(cellFsOpendir); REG(cellFsReaddir); REG(cellFsClosedir); REG(cellFsMkdir);
    REG(cellFsRmdir); REG(cellFsUnlink); REG(cellFsFsync); REG(cellFsChmod); REG(cellFsUtime);
    REG(cellSysmoduleInitialize); REG(cellSysmoduleFinalize); REG(cellSysmoduleSetMemcontainer);
    REG(cellSysmoduleLoadModule); REG(cellSysmoduleUnloadModule);
    REG(cellGameBootCheck); REG(cellGamePatchCheck); REG(cellGameContentPermit); REG(cellGameGetParamInt);
    REG(cellGameGetParamString);
    REG(cellSysutilRegisterCallback); REG(cellSysutilUnregisterCallback); REG(cellSysutilCheckCallback);
    REG(cellSysutilGetSystemParamInt); REG(cellSysutilGetSystemParamString); REG(cellSysutilEnableBgmPlayback);
    REG(cellSysutilDisableBgmPlayback); REG(cellSysCacheMount);
    REG(cellVideoOutGetState); REG(cellVideoOutGetResolution); REG(cellVideoOutGetResolutionAvailability);
    REG(cellVideoOutConfigure); REG(cellVideoOutSetGamma); REG(cellAudioOutGetSoundAvailability);
    REG(cellAudioOutGetNumberOfDevice); REG(cellAudioOutGetDeviceInfo); REG(cellAudioOutConfigure);
    REG(cellRtcGetCurrentTick); REG(cellRtcGetCurrentClockLocalTime); REG(cellRtcGetDayOfWeek);
}
