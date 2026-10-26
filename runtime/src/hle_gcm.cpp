// cellGcmSys + a minimal RSX: FIFO consumer, semaphores/labels, flips,
// surface clears, presented in a Win32 window. No 3D rendering yet.
#include "runtime.h"
#include "rsx/rsx.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#define REG(n) hle_register(#n, hle_##n)

namespace {
constexpr uint32_t CTX_ADDR = mem::RSX_IO + 0x0000;       // CellGcmContextData
constexpr uint32_t CTRL_ADDR = mem::RSX_IO + 0x100040;    // CellGcmControl {put, get, ref}
constexpr uint32_t LABEL_ADDR = mem::RSX_IO + 0x300000;   // 256 labels x 16 bytes
constexpr uint32_t OFFSET_TABLES = mem::RSX_IO + 0x400000; // io->ea and ea->io u16 tables
constexpr uint32_t FRAGMENT = 32 * 1024;
constexpr uint32_t GCM_FLIP_METHOD = 0xfeac;

struct Display { uint32_t offset, pitch, width, height; };
Display g_disp[8];
uint32_t g_io_base, g_io_size;
uint32_t g_io_to_ea[512];       // io offset >> 20 -> ea >> 20 (0xffff = unmapped)
uint32_t g_io_next;             // next free io offset (MB granularity)
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
    if (n++ % 60 == 0) LOG("gcm", "flip #%u (buffer %u)", n, id);
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
        for (; ld32(a) != arg && spin < 2000; spin++) Sleep(1);
        if (spin == 2000)
            LOG("rsx", "semaphore acquire timed out: label 0x%x = 0x%x, want 0x%x (fifo get 0x%x)",
                g_rsx.semaphore_offset, ld32(a), arg, ld32(CTRL_ADDR + 4));
        break;
    }
    case 0x006c: st32(LABEL_ADDR + g_rsx.semaphore_offset, arg); break; // NV406E_SEMAPHORE_RELEASE
    case 0x1d6c: g_rsx.be_semaphore_offset = arg; break;
    case 0x1d70:                                                        // BACK_END_WRITE_SEMAPHORE_RELEASE
        st32(LABEL_ADDR + g_rsx.be_semaphore_offset, (arg & 0xff00ff00) | ((arg & 0xff) << 16) | ((arg >> 16) & 0xff));
        break;
    case 0x1d74: st32(LABEL_ADDR + g_rsx.be_semaphore_offset, arg); break; // TEXTURE_READ_SEMAPHORE_RELEASE
    case GCM_FLIP_METHOD: flip(arg); break;
    case 0xeb00: {                                                      // GCM_SET_USER_COMMAND
        static std::atomic<int> logs{0};
        if (logs++ < 5) LOG("rsx", "user command 0x%x (handler 0x%x)", arg, g_user_handler);
        {
            std::lock_guard<std::mutex> lk(g_intr_lock);
            g_user_commands.push_back(arg);
        }
        g_intr_cv.notify_one();
        break;
    }
    }
}

void fifo_thread() {
    while (!g_hwnd) Sleep(1);
    rsx::init(g_hwnd);
    int trace = getenv("KZ2_TRACE_RSX") ? 400 : 0;
    uint32_t last_put = 0;
    for (;;) {
        uint32_t get = ld32(CTRL_ADDR + 4), put = ld32(CTRL_ADDR);
        if (get == put) { Sleep(0); continue; }
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
            for (uint32_t i = 0; i < count; i++) method(noinc ? m : m + 4 * i, ld32(ea + 4 + 4 * i));
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
                if (g_user_handler) { ctx->r[3] = c; ppu_invoke(ctx, g_user_handler); }
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
    g_io_next = (io_size + 0xfffff) & ~0xfffffu;
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
    uint32_t ea = ARG32(0), size = ARG32(1);
    uint32_t io = g_io_next;
    map_io(ea, io, size);
    g_io_next += (size + 0xfffff) & ~0xfffffu;
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
HLE(cellGcmGetLabelAddress) { RET(LABEL_ADDR + 16 * ARG32(0)); }
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
HLE(cellGcmUnmapIoAddress) { RET(0); }
HLE(cellGcmUnmapEaIoAddress) { RET(0); }
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
