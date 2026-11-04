// cellGcmSys + a minimal RSX: FIFO consumer, semaphores/labels, flips,
// surface clears, presented in a Win32 window. No 3D rendering yet.
#include "runtime.h"
#include "rsx/rsx.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdarg>
#include "idle_backoff.h"
#include "rsx/rsx.h"

// Frame timeline (around an F12 capture): timestamped events of the RSX, the user
// handler and the waiting PPU, to see which step holds up the frame.
// F9: timeline only (no surface dumps, which distort the frame after the flip),
// for the next few frames.
std::atomic<uint32_t> g_tl_flips{0};
std::atomic<uint32_t> g_parked_ea{0};   // guest address of the jump the RSX is parked on (0: not parked)
std::atomic<uint32_t> g_flip_count{0};
void gcm_tl(const char *fmt, ...) {
    if (!rsx::capturing() && g_flip_count.load() >= g_tl_flips.load()) return;
    static uint64_t t0;
    uint64_t now = host_now_ns();
    if (!t0 || now - t0 > 2000000000ull) t0 = now;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    LOG("tl", "%9.3f ms  %s", (now - t0) / 1e6, buf);
}
#define tl gcm_tl
uint32_t gcm_parked_ea() { return g_parked_ea.load(); }
void gcm_parked_wake(uint32_t ea, uint32_t size) {
    uint32_t pe = g_parked_ea.load();
    if (pe && pe - ea < size) WakeByAddressAll(mem::ptr(pe));
}

#define REG(n) hle_register(#n, hle_##n)

namespace spu { std::string stats_line(double secs, uint32_t frames); }
std::string spurs_stats_line(double frames);
std::string fiber_stats();   // fibers.cpp

namespace {
constexpr uint32_t CTX_ADDR = mem::RSX_IO + 0x0000;       // CellGcmContextData
constexpr uint32_t CTRL_ADDR = mem::RSX_IO + 0x100040;    // CellGcmControl {put, get, ref}
constexpr uint32_t LABEL_ADDR = mem::RSX_IO + 0x300000;   // 256 labels x 16 bytes
constexpr uint32_t OFFSET_TABLES = mem::RSX_IO + 0x400000; // io->ea and ea->io u16 tables
constexpr uint32_t FRAGMENT = 32 * 1024;
constexpr uint32_t GCM_FLIP_METHOD = 0xfeac;

// Time the FIFO spent waiting in SEMAPHORE_ACQUIRE (reported with the fps).
std::atomic<uint64_t> g_acquire_us{0};
std::atomic<uint64_t> g_fifo_idle_us{0};   // FIFO empty: waiting for the game to submit
std::atomic<uint64_t> g_fifo_parked_us{0}; // of that: parked on a jump to itself (released by the user handler)
uint64_t g_acquire_max_us;
uint32_t g_acquire_max_label;

struct Display { uint32_t offset, pitch, width, height; };
Display g_disp[8];
uint32_t g_io_base, g_io_size;
uint32_t g_io_to_ea[512];       // io offset >> 20 -> ea >> 20 (0xffff = unmapped)
std::map<uint32_t, uint32_t> g_io_maps;   // first io page -> page count, of each cellGcmMapMainMemory
uint32_t g_flip_handler, g_vblank_handler, g_user_handler;
std::atomic<uint32_t> g_pending_flip_handler{0};
// GCM user commands (method 0xEB00): the RSX raises an interrupt and the
// user handler runs with the command's argument, on the interrupt thread.
std::mutex g_intr_lock;
std::condition_variable g_intr_cv;
std::vector<uint32_t> g_user_commands;
std::atomic<uint32_t> g_last_flip{0xffffffff};
std::atomic<bool> g_started{false};
HWND g_hwnd;

uint32_t io_ea(uint32_t off) {
    uint32_t page = g_io_to_ea[(off >> 20) & 511];
    return page == 0xffff ? 0 : (page << 20) | (off & 0xfffff);
}
uint32_t local_ea(uint32_t off) { return mem::LOCAL_BASE + off; }

void unmap_io_pages(uint32_t first, uint32_t count) {
    for (uint32_t p = first; p < first + count && p < 512; p++) {
        if (g_io_to_ea[p] != 0xffff) st16(OFFSET_TABLES + 0x400 + 2 * g_io_to_ea[p], 0xffff);   // ioAddress table
        st16(OFFSET_TABLES + 2 * p, 0xffff);                                                     // eaAddress table
        g_io_to_ea[p] = 0xffff;
    }
}

void map_io(uint32_t ea, uint32_t io, uint32_t size) {
    for (uint32_t i = 0; i < (size + 0xfffff) >> 20; i++) {
        g_io_to_ea[(io >> 20) + i] = (ea >> 20) + i;
        st16(OFFSET_TABLES + 2 * ((io >> 20) + i), (uint16_t)((ea >> 20) + i));           // eaAddress table
        st16(OFFSET_TABLES + 0x400 + 2 * ((ea >> 20) + i), (uint16_t)((io >> 20) + i));   // ioAddress table
    }
}

/* ---------------- window / present ---------------- */
LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) { LOG("gcm", "window closed - exiting"); ExitProcess(0); }
    if (m == WM_KEYDOWN && w == VK_F12) rsx::request_capture();
    if (m == WM_KEYDOWN && w == VK_F9) { g_tl_flips = g_flip_count.load() + 4; LOG("tl", "F9: timeline for 4 frames"); }
    if (m == WM_KEYDOWN && w == VK_F11) rsx::toggle_cube_maps();
    return DefWindowProcW(h, m, w, l);
}

void window_thread() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"kz2recomp";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r{0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowW(L"kz2recomp", L"Killzone 2 (recomp)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                           CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

void flip(uint32_t id) {
    static uint32_t n;
    // Every 60 flips or 5 s: how fast the game is presenting, and the SPU work behind it.
    static auto last = std::chrono::steady_clock::now();
    static uint32_t last_n = 0;
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - last).count();
    if (++n - last_n >= 60 || secs >= 5) {
        uint32_t frames = n - last_n;
        last = now, last_n = n;
        LOG("gcm", "flip #%u (buffer %u), %.1f fps; %s; RSX FIFO idle %.0f ms/frame (parked %.0f), semaphore waits %.0f ms/frame (longest %.0f ms, label 0x%x); %s",
            n, id, frames / secs, spu::stats_line(secs, frames).c_str(), g_fifo_idle_us.exchange(0) / 1000.0 / frames, g_fifo_parked_us.exchange(0) / 1000.0 / frames,
            g_acquire_us.exchange(0) / 1000.0 / frames, g_acquire_max_us / 1000.0, g_acquire_max_label,
            (rsx::stats_line(frames) + ", " + fiber_stats() + "; " + mem::host_regions()).c_str());
        g_acquire_max_us = 0;
        LOG("gcm", "   PPU time in OS calls (ms/frame): %s", thread::call_report("", frames).c_str());
        LOG("gcm", "   SPURS workloads per frame: %s", spurs_stats_line(frames).c_str());
        if (std::string t = tb_report(frames); !t.empty()) LOG("gcm", "   timebase reads per frame by site:%s", t.c_str());
    }
    // Window title: fps and frame time, averaged over at least a second, and the
    // process's memory (private bytes: what it has committed, as Task Manager shows).
    static auto title_last = now;
    static uint32_t title_n = 0;
    double title_secs = std::chrono::duration<double>(now - title_last).count();
    if (title_secs >= 1 && g_hwnd) {
        uint32_t frames = n - title_n;
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc);
        wchar_t title[128];
        swprintf(title, 128, L"Killzone 2 (recomp) - %.1f fps (%.0f ms) - RAM %.1f GB", frames / title_secs,
                 title_secs * 1000 / frames, pmc.PrivateUsage / 1073741824.0);
        SetWindowTextW(g_hwnd, title);
        title_last = now, title_n = n;
    }
    g_last_flip = id;
    if (id < 8) rsx::flip(g_disp[id].offset, g_disp[id].width, g_disp[id].height, g_disp[id].pitch);
    if (g_flip_handler) g_pending_flip_handler = 1;
}

/* ---------------- RSX FIFO ---------------- */
struct Rsx {
    uint32_t semaphore_offset = 0, be_semaphore_offset = 0;
    uint32_t call_stack[8];
    int call_depth = 0;
    uint64_t methods = 0;
} g_rsx;

std::atomic<uint32_t> g_method_hist[0x4000];
std::atomic<uint32_t> g_cur_method{0}, g_cur_arg{0};

uint32_t g_frame;
FILE *g_frame_dump;

void method(uint32_t m, uint32_t arg) {
    g_rsx.methods++;
    g_method_hist[(m >> 2) & 0x3fff]++;
    if (g_frame_dump) fprintf(g_frame_dump, "%04x %08x\n", m, arg);
    g_cur_method = m, g_cur_arg = arg;
    rsx::method(m, arg);
    g_cur_method = 0xffffffff;
    if (m == GCM_FLIP_METHOD) {
        static int want = getenv("KZ2_DUMP_FRAME") ? atoi(getenv("KZ2_DUMP_FRAME")) : -1;
        g_frame++;
        if (g_frame_dump) { fclose(g_frame_dump); g_frame_dump = nullptr; LOG("rsx", "frame dump written"); }
        if ((int)g_frame == want) g_frame_dump = fopen("frame.txt", "w");
    }
    switch (m) {
    case 0x0050: st32(CTRL_ADDR + 8, arg); break;                       // NV406E_SET_REFERENCE
    case 0x0064: g_rsx.semaphore_offset = arg; break;                   // NV406E_SEMAPHORE_OFFSET
    case 0x0068: {                                                      // NV406E_SEMAPHORE_ACQUIRE
        uint32_t a = LABEL_ADDR + g_rsx.semaphore_offset;
        int spin = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (; ld32(a) != arg && spin < 2000; spin++) Sleep(1);
        if (spin) {
            uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
            g_acquire_us += us;
            if (us > g_acquire_max_us) g_acquire_max_us = us, g_acquire_max_label = g_rsx.semaphore_offset;
        }
        if (spin == 2000)
            LOG("rsx", "semaphore acquire timed out: label 0x%x = 0x%x, want 0x%x (fifo get 0x%x)",
                g_rsx.semaphore_offset, ld32(a), arg, ld32(CTRL_ADDR + 4));
        if (rsx::capturing())
            LOG("sync", "RSX acquire label 0x%x == 0x%x: waited %d ms", g_rsx.semaphore_offset, arg, spin);
        break;
    }
    case 0x006c:                                                        // NV406E_SEMAPHORE_RELEASE
        if (rsx::capturing()) LOG("sync", "RSX release label 0x%x = 0x%x", g_rsx.semaphore_offset, arg);
        st32(LABEL_ADDR + g_rsx.semaphore_offset, arg);
        tl("rsx label 0x%x = 0x%x", g_rsx.semaphore_offset, arg);
        break;
    case 0x1d6c: g_rsx.be_semaphore_offset = arg; break;
    case 0x1d70:                                                        // BACK_END_WRITE_SEMAPHORE_RELEASE
        st32(LABEL_ADDR + g_rsx.be_semaphore_offset, (arg & 0xff00ff00) | ((arg & 0xff) << 16) | ((arg >> 16) & 0xff));
        tl("rsx label 0x%x = 0x%x (back end)", g_rsx.be_semaphore_offset, arg);
        break;
    case 0x1d74: st32(LABEL_ADDR + g_rsx.be_semaphore_offset, arg); tl("rsx label 0x%x = 0x%x (tex)", g_rsx.be_semaphore_offset, arg); break;
    case GCM_FLIP_METHOD: tl("rsx flip 0x%x", arg); flip(arg); g_flip_count++; break;
    case 0xeb00: {                                                      // GCM_SET_USER_COMMAND
        static std::atomic<int> logs{0};
        if (logs++ < 5) LOG("rsx", "user command 0x%x (handler 0x%x)", arg, g_user_handler);
        {
            std::lock_guard<std::mutex> lk(g_intr_lock);
            g_user_commands.push_back(arg);
        }
        g_intr_cv.notify_one();
        tl("rsx user command 0x%x", arg);
        break;
    }
    }
}

void fifo_thread() {
    name_host_thread("rsx fifo");
    while (!g_hwnd) Sleep(1);
    rsx::init(g_hwnd);
    int trace = getenv("KZ2_TRACE_RSX") ? 400 : 0;
    uint32_t last_put = 0, idle = 0;
    for (;;) {
        uint32_t get = ld32(CTRL_ADDR + 4), put = ld32(CTRL_ADDR);
        // Nothing to do: the buffer is empty, or the game parked the RSX on a jump to
        // itself (it rewrites that word from its user-command handler to release it).
        uint32_t ea0 = get != put ? io_ea(get) : 0;
        bool parked = ea0 && ld32(ea0) == (0x20000000 | get) || ea0 && ld32(ea0) == (get | 1);
        if (get == put || parked) {
            if (!idle) tl("rsx %s at 0x%x (ea 0x%x)", parked ? "parked" : "empty", get, ea0), g_parked_ea = parked ? ea0 : 0;
            auto t0 = std::chrono::steady_clock::now();
            // Parked: block on the jump word itself; whoever rewrites it wakes us
            // (gcm_parked_wake). The former 50 us timer sleeps were half a core of kernel time
            // for a thread doing nothing. The timeout covers writers that do not wake us.
            // KZ2_RSX_PARK_WAIT=0: timer sleeps.
            static const bool park_wait = !getenv("KZ2_RSX_PARK_WAIT") || atoi(getenv("KZ2_RSX_PARK_WAIT"));
            if (parked && park_wait && idle >= 64) {
                uint32_t raw = *(volatile uint32_t *)mem::ptr(ea0);
                WaitOnAddress(mem::ptr(ea0), &raw, 4, 1);
                idle++;
            } else host_backoff(idle++);
            uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
            g_fifo_idle_us += us;
            if (parked) g_fifo_parked_us += us;
            continue;
        }
        // Experiment (KZ2_RSX_RESUME_DELAY_MS): after idling, give in-flight SPU writes
        // time to land before running the commands just released - tests whether
        // the RSX overtakes data the SPUs write after releasing it.
        static const int resume_ms = getenv("KZ2_RSX_RESUME_DELAY_MS") ? atoi(getenv("KZ2_RSX_RESUME_DELAY_MS")) : 0;
        if (idle && resume_ms > 0) {
            host_sleep_us(resume_ms * 1000);
            idle = 0;
            continue;   // re-read put/get: more may have been released meanwhile
        }
        if (idle) tl("rsx resumes at 0x%x (put 0x%x), word now 0x%08x", get, put, ld32(io_ea(get))), g_parked_ea = 0;
        idle = 0;
        if (trace && put != last_put) { LOG("rsx", "put=0x%x get=0x%x", put, get); last_put = put; }
        uint32_t ea = io_ea(get);
        if (!ea) { LOG("rsx", "FIFO get 0x%x is not io-mapped (put 0x%x); stalling", get, put); Sleep(1000); continue; }
        uint32_t cmd = ld32(ea);
        if (trace && trace-- > 0) LOG("rsx", "  [0x%x] %08x", get, cmd);
        if ((cmd & 0xe0000003) == 0x20000000) { get = cmd & 0x1ffffffc; }          // old jump
        else if ((cmd & 3) == 1) { get = cmd & 0xfffffffc; }                         // new jump
        else if ((cmd & 3) == 2) {                                                   // call
            if (g_rsx.call_depth < 8) g_rsx.call_stack[g_rsx.call_depth++] = get + 4;
            get = cmd & 0xfffffffc;
        } else if ((cmd & 0xffff0003) == 0x00020000) {                                            // return
            get = g_rsx.call_depth ? g_rsx.call_stack[--g_rsx.call_depth] : get + 4;
        } else {
            uint32_t count = (cmd >> 18) & 0x7ff, m = cmd & 0xfffc;
            bool noinc = cmd & 0x40000000;
            // KZ2_TRACE_METHOD=<method>[:value]: where in memory the command words for it are.
            static const uint32_t tm = getenv("KZ2_TRACE_METHOD") ? strtoul(getenv("KZ2_TRACE_METHOD"), nullptr, 16) : 0;
            static const char *tv = tm && strchr(getenv("KZ2_TRACE_METHOD"), ':') ? strchr(getenv("KZ2_TRACE_METHOD"), ':') + 1 : nullptr;
            static int traced;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t mi = noinc ? m : m + 4 * i, v = ld32(ea + 4 + 4 * i);
                if (tm && mi == tm && (!tv || v == strtoul(tv, nullptr, 16)) && traced++ < 30)
                    LOG("rsx", "method 0x%x = 0x%x from command word at ea 0x%x (fifo get 0x%x)", mi, v, ea + 4 + 4 * i, get);
                method(mi, v);
            }
            get += 4 + 4 * count;
        }
        st32(CTRL_ADDR + 4, get);
    }
}

// Runs on a guest thread so handlers can be invoked with a valid context.
void vblank_loop(PPUContext *ctx) {
    auto next_vblank = std::chrono::steady_clock::now();
    for (;;) {
        next_vblank += std::chrono::milliseconds(16);
        // Until the next vblank, service user commands as the RSX raises them.
        for (;;) {
            std::vector<uint32_t> cmds;
            {
                std::unique_lock<std::mutex> lk(g_intr_lock);
                if (!g_intr_cv.wait_until(lk, next_vblank, [] { return !g_user_commands.empty(); })) break;
                cmds.swap(g_user_commands);
            }
            for (uint32_t c : cmds)
                if (g_user_handler) {
                    tl("handler(0x%x) starts", c);
                    ctx->r[3] = c;
                    ppu_invoke(ctx, g_user_handler);
                    tl("handler(0x%x) done", c);
                    if (uint32_t pe = g_parked_ea.load()) gcm_parked_wake(pe, 4);   // handlers release parked jumps
                }
        }
        if (g_vblank_handler) { ctx->r[3] = 1; ppu_invoke(ctx, g_vblank_handler); }
        if (g_pending_flip_handler.exchange(0) && g_flip_handler) { ctx->r[3] = 1; ppu_invoke(ctx, g_flip_handler); }
    }
}

// Command buffer full: wrap to the start of the fragment once the RSX has caught up.
void gcm_callback(PPUContext *ctx) {
    uint32_t c = ARG32(0);
    uint32_t begin = ld32(c), cur = ld32(c + 8);
    uint32_t begin_off = begin - g_io_base;
    st32(cur, 0x20000000 | begin_off);                 // jump back to begin
    st32(CTRL_ADDR, cur - g_io_base);                  // flush up to the jump
    while (ld32(CTRL_ADDR + 4) != cur - g_io_base && ld32(CTRL_ADDR + 4) != begin_off) Sleep(0);
    st32(CTRL_ADDR, begin_off);
    while (ld32(CTRL_ADDR + 4) != begin_off) Sleep(0);
    st32(c + 8, begin);
    RET(0);
}

void start_threads() {
    if (g_started.exchange(true)) return;
    std::thread(window_thread).detach();
    std::thread(fifo_thread).detach();
    thread::create(hle_make_opd(vblank_loop), 0, 0, 0x10000, "gcm_vblank", false);
}
} // namespace

namespace rsx {
uint32_t io_ea(uint32_t off) { return ::io_ea(off); }
}

void rsx_dump_methods() {
    std::vector<std::pair<uint32_t, uint32_t>> v;
    for (uint32_t i = 0; i < 0x4000; i++)
        if (uint32_t n = g_method_hist[i]) v.push_back({n, i << 2});
    std::sort(v.rbegin(), v.rend());
    std::string s;
    for (size_t k = 0; k < v.size() && k < 24; k++) s += " " + std::to_string(v[k].first) + "x" + [&] { char b[8]; snprintf(b, 8, "%04x", v[k].second); return std::string(b); }();
    LOG("watchdog", "  rsx methods (%llu total):%s", (unsigned long long)g_rsx.methods, s.c_str());
    LOG("watchdog", "  rsx user commands %u, semaphore release %u, back-end release %u", g_method_hist[0xeb00 >> 2].load(),
        g_method_hist[0x006c >> 2].load(), g_method_hist[0x1d70 >> 2].load());
    LOG("watchdog", "  fifo get=0x%x put=0x%x, in method 0x%x (arg 0x%x)", ld32(CTRL_ADDR + 4), ld32(CTRL_ADDR),
        g_cur_method.load(), g_cur_arg.load());
    if (uint32_t ea = io_ea(ld32(CTRL_ADDR + 4))) {
        std::string w;
        for (int i = 0; i < 8; i++) { char b[12]; snprintf(b, 12, " %08x", ld32(ea + 4 * i)); w += b; }
        LOG("watchdog", "  words at get (ea 0x%x):%s", ea, w.c_str());
        for (int row = -4; row < 0; row++) {                // what led up to it
            w.clear();
            for (int i = 0; i < 8; i++) { char b[12]; snprintf(b, 12, " %08x", ld32(ea + 32 * row + 4 * i)); w += b; }
            LOG("watchdog", "  words at get%+d:%s", 32 * row, w.c_str());
        }
    }
    // KZ2_WATCHDOG_PEEK="ea:len,ea:len" dumps guest memory (hex, len rounded up to 32).
    if (const char *p = getenv("KZ2_WATCHDOG_PEEK")) {
        for (const char *s = p; *s;) {
            char *end;
            uint32_t a = strtoul(s, &end, 16), n = *end == ':' ? strtoul(end + 1, &end, 16) : 32;
            for (uint32_t o = 0; o < n; o += 32) {
                std::string w;
                for (int i = 0; i < 8; i++) { char b[12]; snprintf(b, 12, " %08x", ld32(a + o + 4 * i)); w += b; }
                LOG("watchdog", "  peek %08x:%s", a + o, w.c_str());
            }
            s = *end == ',' ? end + 1 : end;
        }
    }
}

HLE(_cellGcmInitBody) {
    // (CellGcmContextData **ctx, u32 cmdSize, u32 ioSize, u32 ioAddress)
    uint32_t cmd_size = ARG32(1), io_size = ARG32(2), io_addr = ARG32(3);
    LOG("gcm", "init cmdSize=0x%x ioSize=0x%x ioAddress=0x%x", cmd_size, io_size, io_addr);
    for (auto &e : g_io_to_ea) e = 0xffff;
    memset(mem::ptr(OFFSET_TABLES), 0xff, 0x400 + 0x1800);
    g_io_base = io_addr, g_io_size = io_size;
    map_io(io_addr, 0, io_size);
    g_io_maps.clear();
    uint32_t frag = cmd_size < FRAGMENT ? cmd_size : FRAGMENT;
    st32(CTX_ADDR + 0, io_addr + 0x1000);              // begin (first 4 KiB reserved)
    st32(CTX_ADDR + 4, io_addr + frag - 4);            // end (room for the wrap jump)
    st32(CTX_ADDR + 8, io_addr + 0x1000);              // current
    st32(CTX_ADDR + 12, hle_make_opd(gcm_callback));   // callback
    st32(CTRL_ADDR, 0x1000);
    st32(CTRL_ADDR + 4, 0x1000);
    st32(CTRL_ADDR + 8, 0);
    st32(ARG32(0), CTX_ADDR);
    start_threads();
    RET(0);
}
HLE(cellGcmGetConfiguration) {
    uint32_t c = ARG32(0);
    st32(c + 0, mem::LOCAL_BASE);
    st32(c + 4, g_io_base);
    st32(c + 8, mem::LOCAL_SIZE);
    st32(c + 12, g_io_size);
    st32(c + 16, 650000000);
    st32(c + 20, 500000000);
    RET(0);
}
HLE(cellGcmAddressToOffset) {
    uint32_t a = ARG32(0), off;
    if (a >= mem::LOCAL_BASE && a < mem::LOCAL_BASE + mem::LOCAL_SIZE) off = a - mem::LOCAL_BASE;
    else {
        uint16_t io = ld16(OFFSET_TABLES + 0x400 + 2 * (a >> 20));
        if (io == 0xffff) { RET(0x802100ff); return; }  // CELL_GCM_ERROR_FAILURE
        off = ((uint32_t)io << 20) | (a & 0xfffff);
    }
    st32(ARG32(1), off);
    RET(0);
}
HLE(cellGcmMapMainMemory) {
    // Like libgcm: the lowest free run of 1 MB io pages (unmapped ranges are reused).
    uint32_t ea = ARG32(0), size = ARG32(1), pages = (size + 0xfffff) >> 20, first = 0, run = 0;
    for (uint32_t p = 0; p < 512 && run < pages; p++) {
        if (g_io_to_ea[p] != 0xffff) { run = 0; first = p + 1; continue; }
        run++;
    }
    if (run < pages) { RET(0x802100ff); return; }                     // CELL_GCM_ERROR_FAILURE
    uint32_t io = first << 20;
    map_io(ea, io, size);
    g_io_maps[first] = pages;
    st32(ARG32(2), io);
    LOG("gcm", "map main memory ea=0x%x size=0x%x -> io 0x%x", ea, size, io);
    RET(0);
}
HLE(cellGcmGetOffsetTable) {
    st32(ARG32(0), OFFSET_TABLES + 0x400);  // ioAddress: indexed by ea >> 20
    st32(ARG32(0) + 4, OFFSET_TABLES);      // eaAddress: indexed by io >> 20
    RET(0);
}
HLE(cellGcmGetControlRegister) { RET(CTRL_ADDR); }
HLE(cellGcmGetLabelAddress) {
    static std::atomic<int> logs{0};
    if (logs++ < 64) LOG("gcm", "cellGcmGetLabelAddress(%u) -> 0x%x, caller lr 0x%llx", ARG32(0), LABEL_ADDR + 16 * ARG32(0),
                         (unsigned long long)ld64(ctx->r[1] + 32));   // the import stub saved its caller's lr
    RET(LABEL_ADDR + 16 * ARG32(0));
}
HLE(cellGcmSetDisplayBuffer) {
    uint32_t id = ARG32(0);
    if (id < 8) g_disp[id] = {ARG32(1), ARG32(2), ARG32(3), ARG32(4)};
    LOG("gcm", "display buffer %u: offset=0x%x pitch=%u %ux%u", id, ARG32(1), ARG32(2), ARG32(3), ARG32(4));
    RET(0);
}
HLE(_cellGcmSetFlipCommand) {
    uint32_t c = ARG32(0), id = ARG32(1);
    uint32_t cur = ld32(c + 8);
    if (cur + 8 > ld32(c + 4)) {
        PPUContext saved = *ctx;
        ppu_invoke(ctx, ld32(c + 12));
        *ctx = saved;
        cur = ld32(c + 8);
    }
    st32(cur, (1u << 18) | GCM_FLIP_METHOD);
    st32(cur + 4, id);
    st32(c + 8, cur + 8);
    RET(0);
}
HLE(cellGcmSetFlipImmediate) { flip(ARG32(0)); RET(0); }
HLE(cellGcmSetFlipHandler) { g_flip_handler = ARG32(0); RET(0); }
HLE(cellGcmSetVBlankHandler) { g_vblank_handler = ARG32(0); RET(0); }
HLE(cellGcmSetUserHandler) { g_user_handler = ARG32(0); RET(0); }
HLE(cellGcmSetGraphicsHandler) { RET(0); }
HLE(cellGcmSetQueueHandler) { RET(0); }
HLE(cellGcmSetFlipMode) { RET(0); }
HLE(cellGcmSetDebugOutputLevel) { RET(0); }
HLE(cellGcmSetTileInfo) { RET(0); }
HLE(cellGcmBindTile) { RET(0); }
HLE(cellGcmUnbindTile) { RET(0); }
HLE(cellGcmBindZcull) { RET(0); }
HLE(cellGcmUnbindZcull) { RET(0); }
// Unmapping frees the io pages of a whole cellGcmMapMainMemory mapping.
int32_t unmap_mapping(uint32_t first_page, const char *how, uint32_t arg) {
    auto it = g_io_maps.find(first_page);
    if (it == g_io_maps.end()) {
        LOG("gcm", "%s 0x%x: not the start of a mapping", how, arg);
        return (int32_t)0x802100ff;
    }
    LOG("gcm", "%s 0x%x: io 0x%x, %u MB", how, arg, first_page << 20, it->second);
    unmap_io_pages(first_page, it->second);
    g_io_maps.erase(it);
    return 0;
}
HLE(cellGcmUnmapIoAddress) { RET(unmap_mapping(ARG32(0) >> 20, "unmap io", ARG32(0))); }
HLE(cellGcmUnmapEaIoAddress) {
    uint16_t io = ld16(OFFSET_TABLES + 0x400 + 2 * (ARG32(0) >> 20));
    RET(io == 0xffff ? (int32_t)0x802100ff : unmap_mapping(io, "unmap ea", ARG32(0)));
}
HLE(cellGcmSortRemapEaIoAddress) { RET(0); }
HLE(_cellGcmFunc15) { RET(0); }

void register_gcm() {
    REG(_cellGcmInitBody); REG(cellGcmGetConfiguration); REG(cellGcmAddressToOffset); REG(cellGcmMapMainMemory);
    REG(cellGcmGetOffsetTable); REG(cellGcmGetControlRegister); REG(cellGcmGetLabelAddress);
    REG(cellGcmSetDisplayBuffer); REG(_cellGcmSetFlipCommand); REG(cellGcmSetFlipImmediate);
    REG(cellGcmSetFlipHandler); REG(cellGcmSetVBlankHandler); REG(cellGcmSetUserHandler);
    REG(cellGcmSetGraphicsHandler); REG(cellGcmSetQueueHandler); REG(cellGcmSetFlipMode);
    REG(cellGcmSetDebugOutputLevel); REG(cellGcmSetTileInfo); REG(cellGcmBindTile); REG(cellGcmUnbindTile);
    REG(cellGcmBindZcull); REG(cellGcmUnbindZcull); REG(cellGcmUnmapIoAddress); REG(cellGcmUnmapEaIoAddress);
    REG(cellGcmSortRemapEaIoAddress); REG(_cellGcmFunc15);
}
