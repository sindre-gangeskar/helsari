// sys_io (pad via XInput), cellAudio (timing + notify events), and
// network/PSN which report "offline".
#include "kernel.h"
#include "keybinds.h"

#include <windows.h>
#include <xinput.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include "idle_backoff.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#define REG(n) hle_register(#n, hle_##n)

/* =================================== pad =================================== */

void dualsense_merge(XINPUT_GAMEPAD &g);   // dualsense.cpp
void mouse_note_controller();               // mouse.cpp

static const bool g_trace_pad = getenv("KZ2_TRACE_PAD") != nullptr;
namespace {
uint16_t axis(SHORT v, bool invert) {
    int x = (v + 32768) >> 8;
    return (uint16_t)(invert ? 255 - x : x);
}

bool window_focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
void keyboard(XINPUT_GAMEPAD &g) {
    if (window_focused()) keybind_apply(g);
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
    if (g_trace_pad) {
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
    const bool pad_there = XInputGetState(0, &s) == ERROR_SUCCESS;
    if (!pad_there) s = {};
    // A controller plugged in at the start: its prompts, until the keyboard or mouse is used.
    static bool first_read = true;
    if (first_read && pad_there) mouse_note_controller();
    first_read = false;
    const XINPUT_GAMEPAD before_dualsense = s.Gamepad;
    dualsense_merge(s.Gamepad);
    const bool from_dualsense = memcmp(&before_dualsense, &s.Gamepad, sizeof before_dualsense) != 0;
    {   // A real controller in use (before the keyboard is added in): the look goes back to the game.
        const XINPUT_GAMEPAD &pad = s.Gamepad;
        const int dead = 8000;
        if (pad.wButtons || pad.bLeftTrigger > 40 || pad.bRightTrigger > 40 || abs(pad.sThumbLX) > dead || abs(pad.sThumbLY) > dead || abs(pad.sThumbRX) > dead || abs(pad.sThumbRY) > dead) {
            keybind_note_pad(from_dualsense);
            mouse_note_controller();
        }
    }
    keyboard(s.Gamepad);
    script(s.Gamepad);
    WORD b = s.Gamepad.wButtons;
    if (g_trace_pad) {
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
// One port: a ring of nBlock blocks of 256 samples (big-endian float, nChannel
// interleaved) the game mixes into ahead of the read index. Every block the
// "hardware" plays the block at the read index, advances it and posts an event to
// every registered notify queue (what the game's audio thread blocks on).
// Playback goes to the default output device through WASAPI (shared mode, the
// port's channel count; Windows converts and down-mixes to the device), and the
// device clock paces the blocks so audio and game stay in step. Without a device,
// a host timer paces them silently. KZ2_NO_AUDIO=1: no output.

namespace {
constexpr uint32_t AUDIO_BLOCK_SAMPLES = 256, MAX_PORTS = 8, OUT_CHANNELS = 8;
struct AudioPort {
    uint32_t addr = 0, read_index = 0, nchannel = 0, nblock = 0;
    std::atomic<bool> open{false}, running{false};
    std::atomic<float> level{1.0f};
};
AudioPort g_ports[MAX_PORTS];
std::vector<uint64_t> g_notify_keys;
std::atomic<bool> g_audio_thread{false};
std::atomic<uint64_t> g_audio_block{0};   // blocks played since start: every port's clock

struct Wasapi { IAudioClient *client = nullptr; IAudioRenderClient *render = nullptr; UINT32 frames = 0; };

bool wasapi_open(uint32_t nch, Wasapi &w) {
    static const GUID clsid_enum = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
    static const GUID iid_enum = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
    static const GUID iid_client = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
    static const GUID iid_render = {0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
    static const GUID ieee_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    if (getenv("KZ2_NO_AUDIO") && atoi(getenv("KZ2_NO_AUDIO"))) return false;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = nullptr;
    IMMDevice *dev = nullptr;
    if (FAILED(CoCreateInstance(clsid_enum, nullptr, CLSCTX_ALL, iid_enum, (void **)&en))) return false;
    HRESULT hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    en->Release();
    if (FAILED(hr)) return false;
    hr = dev->Activate(iid_client, CLSCTX_ALL, nullptr, (void **)&w.client);
    dev->Release();
    if (FAILED(hr)) return false;
    WAVEFORMATEXTENSIBLE fmt{};
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = (WORD)nch;
    fmt.Format.nSamplesPerSec = 48000;
    fmt.Format.wBitsPerSample = 32;
    fmt.Format.nBlockAlign = (WORD)(nch * 4);
    fmt.Format.nAvgBytesPerSec = 48000 * nch * 4;
    fmt.Format.cbSize = sizeof fmt - sizeof fmt.Format;
    fmt.Samples.wValidBitsPerSample = 32;
    // PS3 order: L R C LFE, then surround left/right, then back left/right (7.1).
    fmt.dwChannelMask = nch == 8 ? 0x63f : nch == 6 ? 0x3f : 0x3;
    fmt.SubFormat = ieee_float;
    hr = w.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                              1000000 /* 100 ms */, 0, (WAVEFORMATEX *)&fmt, nullptr);
    if (FAILED(hr) || FAILED(w.client->GetBufferSize(&w.frames)) ||
        FAILED(w.client->GetService(iid_render, (void **)&w.render)) || FAILED(w.client->Start())) {
        LOG("audio", "WASAPI output unavailable (0x%lx): audio is timed but silent", (long)hr);
        if (w.render) w.render->Release(), w.render = nullptr;
        w.client->Release(), w.client = nullptr;
        return false;
    }
    LOG("audio", "WASAPI output: %u channels, 48 kHz, device buffer %u frames", nch, w.frames);
    return true;
}

// Mixes every running port's block at the read index into one 7.1 stream (2-channel
// ports onto front left/right), scaled by the port level, then advances all ports.
void audio_thread() {
    Wasapi w;
    bool out = wasapi_open(OUT_CHANNELS, w);
    const UINT32 target = 8 * AUDIO_BLOCK_SAMPLES;   // ~43 ms queued at the device: rides out CPU spikes
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    std::vector<float> mix(OUT_CHANNELS * AUDIO_BLOCK_SAMPLES);
    // Diagnostics, logged every ~5 s: device underruns (queue empty when we came back),
    // stale blocks (a port block unchanged since it was last played: the game did not
    // refill it in time), clipped samples (mix beyond full scale).
    uint32_t st_under = 0, st_stale = 0, st_clip = 0, st_blocks = 0;
    float st_peak = 0;
    std::vector<uint64_t> played_hash(MAX_PORTS * 64, 0);
    for (;;) {
        uint64_t block = g_audio_block.load();
        if (out) {
            UINT32 pad = 0;
            if (FAILED(w.client->GetCurrentPadding(&pad))) out = false;
            else if (pad > target) { host_sleep_us(1000); continue; }
            else if (pad == 0 && block > 16) st_under++;
        }
        if (!out) {   // no device: pace by the clock
            double due = (double)(block + 1) * AUDIO_BLOCK_SAMPLES / 48000.0;
            do { Sleep(1); QueryPerformanceCounter(&t); } while ((double)(t.QuadPart - t0.QuadPart) / f.QuadPart < due);
        }
        if (out) {
            // Diagnostics: a port block unchanged since it was last played (counted only).
            bool stale = false;
            uint64_t hashes[MAX_PORTS] = {};
            for (auto &p : g_ports) {
                if (!p.open || !p.running || !p.nblock || !p.nchannel) continue;
                const uint32_t n = p.nchannel;
                const uint32_t *src = (const uint32_t *)mem::ptr(p.addr + (uint32_t)(block % p.nblock) * n * AUDIO_BLOCK_SAMPLES * 4);
                uint64_t h = 1469598103934665603ull;
                bool nz = false;
                for (uint32_t i = 0; i < n * AUDIO_BLOCK_SAMPLES; i++) h = (h ^ src[i]) * 1099511628211ull, nz |= src[i] != 0;
                hashes[&p - g_ports] = h;
                if (nz && h == played_hash[(&p - g_ports) * 64 + (block % p.nblock) % 64]) stale = true;
            }
            // (Holding playback until a stale block was refilled broke audio: a port the game
            // leaves idle keeps old non-silent samples, so every block looked stale.)
            if (stale) st_stale++;
            for (auto &p : g_ports)
                if (p.open && p.running && p.nblock && p.nchannel)
                    played_hash[(&p - g_ports) * 64 + (block % p.nblock) % 64] = hashes[&p - g_ports];
            std::fill(mix.begin(), mix.end(), 0.0f);
            bool any = false;
            for (auto &p : g_ports) {
                if (!p.open || !p.running || !p.nblock || !p.nchannel) continue;
                const uint32_t n = p.nchannel;
                const uint32_t *src = (const uint32_t *)mem::ptr(p.addr + (uint32_t)(block % p.nblock) * n * AUDIO_BLOCK_SAMPLES * 4);
                const float lv = p.level.load();
                for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
                    for (uint32_t c = 0; c < n && c < OUT_CHANNELS; c++) {
                        uint32_t u = __builtin_bswap32(src[i * n + c]);
                        float v;
                        memcpy(&v, &u, 4);
                        mix[i * OUT_CHANNELS + c] += v * lv;
                    }
                any = true;
            }
            // Soft limiter: transparent below 0.8, then a smooth knee that never exceeds 1.0
            // (hard clipping at the device is what made loud moments distort).
            for (float &v : mix) {
                float a = v < 0 ? -v : v;
                if (a > 1.0f) st_clip++;
                if (a > st_peak) st_peak = a;
                if (a > 0.8f) {
                    float x = (a - 0.8f) / 0.2f;                   // 0.. over the knee
                    float y = 0.8f + 0.2f * (x / (1.0f + x));      // -> 1.0 asymptotically
                    v = v < 0 ? -y : y;
                }
            }
            if (++st_blocks == 940) {   // ~5 s
                LOG("audio", "last 5 s: %u underruns, %u stale blocks, %u samples over full scale (limited), peak %.2f", st_under, st_stale,
                    st_clip, st_peak);
                st_under = st_stale = st_clip = st_blocks = 0, st_peak = 0;
            }
            BYTE *dst = nullptr;
            if (SUCCEEDED(w.render->GetBuffer(AUDIO_BLOCK_SAMPLES, &dst))) {
                if (any) memcpy(dst, mix.data(), mix.size() * 4);
                w.render->ReleaseBuffer(AUDIO_BLOCK_SAMPLES, any ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
            }
        }
        block = g_audio_block.fetch_add(1) + 1;
        for (auto &p : g_ports)
            if (p.open && p.running && p.nblock) st64(p.read_index, block % p.nblock);
        for (uint64_t key : g_notify_keys)
            if (uint32_t q = kern::equeue_by_key(key)) kern::equeue_push(q, {0, 0, 0, 0});
    }
}
AudioPort *port(uint32_t id) { return id < MAX_PORTS && g_ports[id].open ? &g_ports[id] : nullptr; }
} // namespace

HLE(cellAudioInit) { RET(0); }
HLE(cellAudioQuit) { RET(0); }
HLE(cellAudioPortOpen) {
    // CellAudioPortParam: u64 nChannel, u64 nBlock, u64 attr, float level
    uint32_t prm = ARG32(0);
    uint32_t id = 0;
    while (id < MAX_PORTS && g_ports[id].open) id++;
    if (id == MAX_PORTS) { RET(0x80310705); return; }   // CELL_AUDIO_ERROR_PORT_FULL
    AudioPort &p = g_ports[id];
    p.nchannel = (uint32_t)ld64(prm);
    p.nblock = (uint32_t)ld64(prm + 8);
    uint32_t size = p.nchannel * p.nblock * AUDIO_BLOCK_SAMPLES * 4;
    if (!p.addr) p.addr = mem::alloc(std::max(size, 8u * 8 * AUDIO_BLOCK_SAMPLES * 4), 0x80);   // reused on reopen
    memset(mem::ptr(p.addr), 0, size);
    if (!p.read_index) p.read_index = mem::alloc(8, 16);
    st64(p.read_index, 0);
    p.level = 1.0f, p.running = false, p.open = true;
    st32(ARG32(1), id);
    LOG("audio", "port %u open: %u channels, %u blocks", id, p.nchannel, p.nblock);
    if (!g_audio_thread.exchange(true)) std::thread(audio_thread).detach();
    RET(0);
}
HLE(cellAudioGetPortConfig) {
    // CellAudioPortConfig: u32 readIndexAddr, u32 status, u64 nChannel, u64 nBlock, u32 portSize, u32 portAddr
    AudioPort *p = port(ARG32(0));
    if (!p) { RET(0x80310709); return; }   // CELL_AUDIO_ERROR_PORT_NOT_OPEN
    uint32_t c = ARG32(1);
    st32(c, p->read_index);
    st32(c + 4, p->running ? 2 : 1);
    st64(c + 8, p->nchannel);
    st64(c + 16, p->nblock);
    st32(c + 24, p->nchannel * p->nblock * AUDIO_BLOCK_SAMPLES * 4);
    st32(c + 28, p->addr);
    RET(0);
}
HLE(cellAudioPortStart) { if (AudioPort *p = port(ARG32(0))) p->running = true; RET(0); }
HLE(cellAudioPortStop) { if (AudioPort *p = port(ARG32(0))) p->running = false; RET(0); }
HLE(cellAudioPortClose) { if (AudioPort *p = port(ARG32(0))) p->running = false, p->open = false; RET(0); }
HLE(cellAudioSetPortLevel) {
    // (u32 port, float level): the level arrives in f1
    if (AudioPort *p = port(ARG32(0))) p->level = (float)ctx->f[1];
    RET(0);
}
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

