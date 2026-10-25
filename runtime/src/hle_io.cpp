// sys_io (pad via XInput), cellAudio (timing + notify events), and
// network/PSN which report "offline".
#include "kernel.h"

#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#define REG(n) hle_register(#n, hle_##n)

/* =================================== pad =================================== */

void dualsense_merge(XINPUT_GAMEPAD &g);   // dualsense.cpp

namespace {
uint16_t axis(SHORT v, bool invert) {
    int x = (v + 32768) >> 8;
    return (uint16_t)(invert ? 255 - x : x);
}

// Keyboard stands in for a pad while our window has focus:
// arrows = d-pad, WASD = left stick, IJKL = right stick, Space/Z = cross,
// X/Esc = circle, C = square, V = triangle, Q/E = L1/R1, 1/3 = L2/R2,
// Enter = start, Backspace = select.
bool key(int vk) { return GetAsyncKeyState(vk) & 0x8000; }
bool window_focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
void keyboard(XINPUT_GAMEPAD &g) {
    if (!window_focused()) return;
    static const struct { int vk; WORD bit; } map[] = {
        {VK_UP, XINPUT_GAMEPAD_DPAD_UP}, {VK_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
        {VK_LEFT, XINPUT_GAMEPAD_DPAD_LEFT}, {VK_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
        {VK_SPACE, XINPUT_GAMEPAD_A}, {'Z', XINPUT_GAMEPAD_A}, {'X', XINPUT_GAMEPAD_B},
        {VK_ESCAPE, XINPUT_GAMEPAD_B}, {'C', XINPUT_GAMEPAD_X}, {'V', XINPUT_GAMEPAD_Y},
        {'Q', XINPUT_GAMEPAD_LEFT_SHOULDER}, {'E', XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {VK_RETURN, XINPUT_GAMEPAD_START}, {VK_BACK, XINPUT_GAMEPAD_BACK},
    };
    for (auto &m : map) if (key(m.vk)) g.wButtons |= m.bit;
    if (key('1')) g.bLeftTrigger = 255;
    if (key('3')) g.bRightTrigger = 255;
    auto stick = [](SHORT &x, SHORT &y, int l, int r, int u, int d) {
        if (key(l)) x = -32768;
        if (key(r)) x = 32767;
        if (key(u)) y = 32767;
        if (key(d)) y = -32768;
    };
    stick(g.sThumbLX, g.sThumbLY, 'A', 'D', 'W', 'S');
    stick(g.sThumbRX, g.sThumbRY, 'J', 'L', 'I', 'K');
}

// KZ2_PAD_SCRIPT="45:a,50:start,52.5:down" holds each button for 0.2 s at
// that many seconds after the first pad read, so menus can be driven without
// a human. "@188:a" instead presses on the 188th pad read for 6 reads, which
// replays a KZ2_TRACE_PAD log exactly regardless of frame rate.
// Names: a b x y start back up down left right lb rb.
struct ScriptPress { double t; WORD bit; bool by_read; };
std::vector<ScriptPress> load_script() {
    std::vector<ScriptPress> v;
    const char *e = getenv("KZ2_PAD_SCRIPT");
    if (!e) return v;
    static const struct { const char *name; WORD bit; } names[] = {
        {"a", XINPUT_GAMEPAD_A}, {"b", XINPUT_GAMEPAD_B}, {"x", XINPUT_GAMEPAD_X}, {"y", XINPUT_GAMEPAD_Y},
        {"start", XINPUT_GAMEPAD_START}, {"back", XINPUT_GAMEPAD_BACK}, {"up", XINPUT_GAMEPAD_DPAD_UP},
        {"down", XINPUT_GAMEPAD_DPAD_DOWN}, {"left", XINPUT_GAMEPAD_DPAD_LEFT},
        {"right", XINPUT_GAMEPAD_DPAD_RIGHT}, {"lb", XINPUT_GAMEPAD_LEFT_SHOULDER},
        {"rb", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    };
    std::string s = e;
    for (size_t i = 0; i < s.size();) {
        size_t end = s.find(',', i);
        if (end == std::string::npos) end = s.size();
        std::string item = s.substr(i, end - i);
        size_t colon = item.find(':');
        if (colon != std::string::npos)
            for (auto &n : names)
                if (item.substr(colon + 1) == n.name)
                    v.push_back({atof(item.c_str() + (item[0] == '@')), n.bit, item[0] == '@'});
        i = end + 1;
    }
    LOG("pad", "script: %zu presses", v.size());
    return v;
}
void script(XINPUT_GAMEPAD &g) {
    static std::vector<ScriptPress> presses = load_script();
    static ULONGLONG t0 = GetTickCount64();
    static int reads;
    double now = (GetTickCount64() - t0) / 1000.0;
    reads++;
    for (auto &p : presses) {
        if (p.by_read ? reads >= p.t && reads < p.t + 6 : now >= p.t && now < p.t + 0.2) g.wButtons |= p.bit;
    }
}
} // namespace

namespace {
uint32_t g_port_setting[7];
bool g_reported_connect;
} // namespace

HLE(cellPadInit) { LOG("pad", "Init(max %u)", ARG32(0)); RET(0); }
HLE(cellPadEnd) { RET(0); }
HLE(cellPadSetPortSetting) {
    LOG("pad", "SetPortSetting(port %u, 0x%x)", ARG32(0), ARG32(1));
    if (ARG32(0) < 7) g_port_setting[ARG32(0)] = ARG32(1);
    RET(0);
}
HLE(cellPadSetActDirect) { RET(0); }
HLE(cellPadGetInfo2) {
    // CellPadInfo2: max_connect, now_connect, system_info, then 7-entry
    // arrays of port_status, port_setting, device_capability, device_type.
    uint32_t p = ARG32(0);
    if (getenv("KZ2_TRACE_PAD")) {
        static int calls;
        if (++calls % 600 == 1) LOG("pad", "GetInfo2 #%d", calls);
    }
    memset(mem::ptr(p), 0, 4 * 3 + 4 * 7 * 4);
    st32(p + 0, 7);                     // max_connect
    st32(p + 4, 1);                     // now_connect: keyboard always counts as a pad
    st32(p + 8, 0);                     // system_info: not intercepted by the XMB
    // port_status[0]: connected, plus ASSIGN_CHANGES the first time, which
    // is how real firmware announces a newly attached pad.
    st32(p + 12, g_reported_connect ? 1 : 1 | 2);
    g_reported_connect = true;
    for (int i = 0; i < 7; i++) st32(p + 12 + 28 + 4 * i, g_port_setting[i]);
    st32(p + 12 + 28 * 2, 1 | 2 | 4);   // device_capability[0]: PS3 conformity, pressure, sensors
    RET(0);
}
HLE(cellPadGetData) {
    uint32_t port = ARG32(0), d = ARG32(1);
    memset(mem::ptr(d), 0, 4 + 2 * 64);
    if (port != 0) { RET(0); return; }
    XINPUT_STATE s{};
    if (XInputGetState(0, &s) != ERROR_SUCCESS) s = {};
    dualsense_merge(s.Gamepad);
    keyboard(s.Gamepad);
    script(s.Gamepad);
    WORD b = s.Gamepad.wButtons;
    if (getenv("KZ2_TRACE_PAD")) {
        static int calls;
        static WORD last;
        if (++calls % 600 == 1 || b != last) LOG("pad", "GetData #%d buttons=0x%04x lr=0x%llx", calls, b, (unsigned long long)ctx->lr);
        last = b;
    }
    uint16_t d1 = 0, d2 = 0;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT) d1 |= 0x80;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN) d1 |= 0x40;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT) d1 |= 0x20;
    if (b & XINPUT_GAMEPAD_DPAD_UP) d1 |= 0x10;
    if (b & XINPUT_GAMEPAD_START) d1 |= 0x08;
    if (b & XINPUT_GAMEPAD_RIGHT_THUMB) d1 |= 0x04;
    if (b & XINPUT_GAMEPAD_LEFT_THUMB) d1 |= 0x02;
    if (b & XINPUT_GAMEPAD_BACK) d1 |= 0x01;
    if (b & XINPUT_GAMEPAD_X) d2 |= 0x80;          // square
    if (b & XINPUT_GAMEPAD_A) d2 |= 0x40;          // cross
    if (b & XINPUT_GAMEPAD_B) d2 |= 0x20;          // circle
    if (b & XINPUT_GAMEPAD_Y) d2 |= 0x10;          // triangle
    if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) d2 |= 0x08;
    if (b & XINPUT_GAMEPAD_LEFT_SHOULDER) d2 |= 0x04;
    if (s.Gamepad.bRightTrigger > 64) d2 |= 0x02;
    if (s.Gamepad.bLeftTrigger > 64) d2 |= 0x01;
    st32(d, 24);                                   // len
    uint32_t btn = d + 4;
    st16(btn + 2 * 1, 0x70 | 12);                  // reserved/length nibble as real pads report
    st16(btn + 2 * 2, d1);
    st16(btn + 2 * 3, d2);
    st16(btn + 2 * 4, axis(s.Gamepad.sThumbRX, false));
    st16(btn + 2 * 5, axis(s.Gamepad.sThumbRY, true));
    st16(btn + 2 * 6, axis(s.Gamepad.sThumbLX, false));
    st16(btn + 2 * 7, axis(s.Gamepad.sThumbLY, true));
    // Pressure mode (requested by the game) reports 0-255 force per button.
    auto press = [&](int i, bool down) { st16(btn + 2 * i, down ? 255 : 0); };
    press(8, b & XINPUT_GAMEPAD_DPAD_RIGHT);
    press(9, b & XINPUT_GAMEPAD_DPAD_LEFT);
    press(10, b & XINPUT_GAMEPAD_DPAD_UP);
    press(11, b & XINPUT_GAMEPAD_DPAD_DOWN);
    press(12, b & XINPUT_GAMEPAD_Y);
    press(13, b & XINPUT_GAMEPAD_B);
    press(14, b & XINPUT_GAMEPAD_A);
    press(15, b & XINPUT_GAMEPAD_X);
    press(16, b & XINPUT_GAMEPAD_LEFT_SHOULDER);
    press(17, b & XINPUT_GAMEPAD_RIGHT_SHOULDER);
    st16(btn + 2 * 18, s.Gamepad.bLeftTrigger);
    st16(btn + 2 * 19, s.Gamepad.bRightTrigger);
    // Motion sensors at rest: x, y (gravity), z, gyro, as RPCS3 reports them.
    st16(btn + 2 * 20, 512);
    st16(btn + 2 * 21, 399);
    st16(btn + 2 * 22, 512);
    st16(btn + 2 * 23, 512);
    RET(0);
}

/* ================================== audio ================================== */
// One port; a host timer advances the read index every 256 samples (5.33 ms)
// and posts an event to every registered notify queue, which is what the
// game's audio thread blocks on. Samples are not played yet.

namespace {
constexpr uint32_t AUDIO_BLOCK_SAMPLES = 256;
struct AudioPort { uint32_t addr, read_index, nchannel, nblock; bool open, running; };
AudioPort g_port;
std::vector<uint64_t> g_notify_keys;
std::atomic<bool> g_audio_thread{false};

void audio_thread() {
    uint64_t block = 0;
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (;;) {
        double due = (double)(block + 1) * AUDIO_BLOCK_SAMPLES / 48000.0;
        do { Sleep(1); QueryPerformanceCounter(&t); } while ((double)(t.QuadPart - t0.QuadPart) / f.QuadPart < due);
        block++;
        if (g_port.running && g_port.nblock) st64(g_port.read_index, block % g_port.nblock);
        for (uint64_t key : g_notify_keys)
            if (uint32_t q = kern::equeue_by_key(key)) kern::equeue_push(q, {0, 0, 0, 0});
    }
}
} // namespace

HLE(cellAudioInit) { RET(0); }
HLE(cellAudioQuit) { RET(0); }
HLE(cellAudioPortOpen) {
    // CellAudioPortParam: u64 nChannel, u64 nBlock, u64 attr, float level
    uint32_t p = ARG32(0);
    g_port.nchannel = (uint32_t)ld64(p);
    g_port.nblock = (uint32_t)ld64(p + 8);
    uint32_t size = g_port.nchannel * g_port.nblock * AUDIO_BLOCK_SAMPLES * 4;
    g_port.addr = mem::alloc(size, 0x80);
    memset(mem::ptr(g_port.addr), 0, size);
    g_port.read_index = mem::alloc(8, 16);
    st64(g_port.read_index, 0);
    g_port.open = true;
    st32(ARG32(1), 0);
    LOG("audio", "port open: %u channels, %u blocks", g_port.nchannel, g_port.nblock);
    if (!g_audio_thread.exchange(true)) std::thread(audio_thread).detach();
    RET(0);
}
HLE(cellAudioGetPortConfig) {
    // CellAudioPortConfig: u32 readIndexAddr, u32 status, u64 nChannel, u64 nBlock, u32 portSize, u32 portAddr
    uint32_t c = ARG32(1);
    st32(c, g_port.read_index);
    st32(c + 4, g_port.running ? 2 : 1);
    st64(c + 8, g_port.nchannel);
    st64(c + 16, g_port.nblock);
    st32(c + 24, g_port.nchannel * g_port.nblock * AUDIO_BLOCK_SAMPLES * 4);
    st32(c + 28, g_port.addr);
    RET(0);
}
HLE(cellAudioPortStart) { g_port.running = true; RET(0); }
HLE(cellAudioPortStop) { g_port.running = false; RET(0); }
HLE(cellAudioPortClose) { g_port.open = false; RET(0); }
HLE(cellAudioSetPortLevel) { RET(0); }
HLE(cellAudioSetNotifyEventQueue) { g_notify_keys.push_back(ARG(0)); RET(0); }
HLE(cellAudioRemoveNotifyEventQueue) { RET(0); }
HLE(cellAudioGetPortTimestamp) { st64(ARG32(2), ppu_timebase() / 80); RET(0); }
HLE(cellAudioGetPortBlockTag) { st64(ARG32(2), ARG(1)); RET(0); }

/* ============================ network / PSN: offline ============================ */

HLE(cellNetCtlInit) { RET(0); }
HLE(cellNetCtlTerm) { RET(0); }
HLE(cellNetCtlGetState) { st32(ARG32(0), 0); RET(0); }   // CELL_NET_CTL_STATE_Disconnected
HLE(cellNetCtlGetInfo) { RET(0x80130106); }              // CELL_NET_CTL_ERROR_NOT_CONNECTED
HLE(sceNpManagerGetNpId) { RET(0x8002aa0d); }            // not signed in
HLE(sceNpManagerGetOnlineName) { RET(0x8002aa0d); }
HLE(sceNpManagerGetTicket) { RET(0x8002aa0d); }
HLE(sceNpManagerRequestTicket2) { RET(0x8002aa0d); }
HLE(sceNpManagerGetAccountRegion) { RET(0x8002aa0d); }
HLE(sceNpManagerGetContentRatingFlag) { RET(0x8002aa0d); }
HLE(sceNpManagerGetChatRestrictionFlag) { RET(0x8002aa0d); }
HLE(socket) { RET(-1); }
HLE(cellMicIsAttached) { RET(0); }
HLE(cellMicOpen) { RET(0x80140105); }                    // no device

void register_io() {
    REG(cellPadInit); REG(cellPadEnd); REG(cellPadSetPortSetting); REG(cellPadSetActDirect);
    REG(cellPadGetInfo2); REG(cellPadGetData);
    REG(cellAudioInit); REG(cellAudioQuit); REG(cellAudioPortOpen); REG(cellAudioGetPortConfig);
    REG(cellAudioPortStart); REG(cellAudioPortStop); REG(cellAudioPortClose); REG(cellAudioSetPortLevel);
    REG(cellAudioSetNotifyEventQueue); REG(cellAudioRemoveNotifyEventQueue); REG(cellAudioGetPortTimestamp);
    REG(cellAudioGetPortBlockTag);
    REG(cellNetCtlInit); REG(cellNetCtlTerm); REG(cellNetCtlGetState); REG(cellNetCtlGetInfo);
    REG(sceNpManagerGetNpId); REG(sceNpManagerGetOnlineName); REG(sceNpManagerGetTicket);
    REG(sceNpManagerRequestTicket2); REG(sceNpManagerGetAccountRegion); REG(sceNpManagerGetContentRatingFlag);
    REG(sceNpManagerGetChatRestrictionFlag); REG(socket); REG(cellMicIsAttached); REG(cellMicOpen);
}

