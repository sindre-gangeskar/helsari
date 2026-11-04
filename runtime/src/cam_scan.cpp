// Finds the view's heading angle in guest memory. F6 turns the view by exact mouse bursts: an angle
// moves linearly (a second equal burst moves it by the same amount, reversing returns it), which
// sines, matrices and bones do not. Each value that passes is then nudged directly, and the static
// view matrix shows whether the camera really followed. Stand still; do not touch the mouse.
#include "runtime.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

extern std::atomic<uint32_t> g_look_entity, g_look_controller;   // mouse.cpp
void mouse_inject(int counts);                                   // mouse.cpp

namespace {

constexpr int SNAPS = 5;            // idle, idle again, after burst, after a second, after reversing both
constexpr int BURST = 300;
constexpr uint32_t CAMERA_EA = 0x11c21c0;
constexpr uint64_t SETTLE_NS = 700000000ull, NUDGE_NS = 350000000ull;
constexpr size_t MAX_NUDGES = 40;

struct Region { uint32_t ea, size; std::vector<uint8_t> s[SNAPS]; };
struct Test { uint32_t ea; float delta; };

std::atomic<bool> g_start{false};
int g_snap = -1;                    // snapshots taken, -1 when not scanning
uint64_t g_until;
std::vector<Region> g_regions;
std::vector<Test> g_tests;
size_t g_test;
bool g_written, g_testing;
float g_before, g_camera_before;

float guest_float(uint32_t ea) {
    const uint32_t u = ld32(ea);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
void guest_float_set(uint32_t ea, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    st32(ea, u);
}

std::string where(uint32_t ea) {
    const uint32_t ent = g_look_entity.load(), ctl = g_look_controller.load();
    char b[48];
    if (ent && ea >= ent && ea < ent + 0x4000) snprintf(b, sizeof b, " (player +0x%x)", ea - ent);
    else if (ctl && ea >= ctl && ea < ctl + 0x4000) snprintf(b, sizeof b, " (controller +0x%x)", ea - ctl);
    else b[0] = 0;
    return b;
}

void list_regions() {
    g_regions.clear();
    uint8_t *base = (uint8_t *)mem::ptr(0);
    for (uint64_t ea = 0x10000; ea < 0xC0000000ull;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(base + ea, &mi, sizeof mi)) break;
        const uint64_t end = std::min<uint64_t>((uint8_t *)mi.BaseAddress - base + mi.RegionSize, 0xC0000000ull);
        if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY)) && !(mi.Protect & PAGE_GUARD)) {
            g_regions.push_back({});
            g_regions.back().ea = (uint32_t)ea, g_regions.back().size = (uint32_t)(end - ea);
        }
        ea = end;
    }
}

void snap() {
    for (auto &r : g_regions) {
        r.s[g_snap].resize(r.size);
        memcpy(r.s[g_snap].data(), mem::ptr(r.ea), r.size);
    }
    g_snap++;
}

float be_float(const std::vector<uint8_t> &v, size_t i) {
    uint32_t u;
    memcpy(&u, &v[i], 4);
    u = __builtin_bswap32(u);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

void analyze() {
    g_tests.clear();
    int found = 0;
    for (auto &r : g_regions) {
        for (size_t i = 0; i + 4 <= r.size; i += 4) {
            float v[SNAPS];
            bool ok = true;
            for (int k = 0; k < SNAPS && ok; k++) {
                v[k] = be_float(r.s[k], i);
                ok = std::isfinite(v[k]) && std::fabs(v[k]) < 100000.f;
            }
            if (!ok) continue;
            const float d1 = v[2] - v[1], d2 = v[3] - v[2];
            if (std::fabs(d1) < 0.02f) continue;
            const float tol = 0.03f * std::fabs(d1);
            if (std::fabs(v[1] - v[0]) > tol || std::fabs(d2 - d1) > tol || std::fabs(v[4] - v[1]) > tol) continue;
            const uint32_t ea = r.ea + (uint32_t)i;
            found++;
            LOG("camscan", "linear 0x%08x%s: %.4f, burst %+.4f, second %+.4f, back %.4f", ea, where(ea).c_str(), v[1], d1, d2, v[4]);
            // Nudge what moved by about the burst itself (0.001 rad a count), from a real value.
            if (g_tests.size() < MAX_NUDGES && std::fabs(d1) > 0.25f && std::fabs(d1) < 0.36f && std::fabs(v[1]) > 0.001f && std::fabs(v[1]) < 7.f)
                g_tests.push_back({ea, d1});
        }
    }
    LOG("camscan", "%d values move linearly", found);
    g_regions.clear();
}

void nudge(uint64_t now) {
    if (now < g_until) return;
    if (!g_written) {
        if (g_test >= g_tests.size()) {
            g_testing = false;
            LOG("camscan", "nudging done");
            return;
        }
        g_before = guest_float(g_tests[g_test].ea);
        g_camera_before = guest_float(CAMERA_EA);
        guest_float_set(g_tests[g_test].ea, g_before + g_tests[g_test].delta);
        g_written = true;
        g_until = now + NUDGE_NS;
        return;
    }
    const Test &t = g_tests[g_test];
    const float after = guest_float(t.ea), camera = guest_float(CAMERA_EA);
    LOG("camscan", "nudged 0x%08x%s by %+.4f: value kept %+.4f, CAMERA moved %+.4f", t.ea, where(t.ea).c_str(), t.delta, after - g_before, camera - g_camera_before);
    if (std::fabs(after - g_before - t.delta) < 0.5f * std::fabs(t.delta)) guest_float_set(t.ea, after - t.delta);
    g_written = false;
    g_test++;
    g_until = now + NUDGE_NS;
}

} // namespace

void cam_scan_start() { g_start = true; }

// Once per pad read.
void cam_scan_step(XINPUT_GAMEPAD &) {
    const uint64_t now = host_now_ns();
    if (g_start.exchange(false) && g_snap < 0 && !g_testing) {
        list_regions();
        g_snap = 0;
        g_until = now + SETTLE_NS;
        LOG("camscan", "start: %zu regions", g_regions.size());
    }
    if (g_testing) { nudge(now); return; }
    if (g_snap < 0 || now < g_until) return;
    snap();
    g_until = now + SETTLE_NS;
    if (g_snap == 2) mouse_inject(BURST);
    if (g_snap == 3) mouse_inject(BURST);
    if (g_snap == 4) mouse_inject(-2 * BURST);
    if (g_snap == SNAPS) {
        analyze();
        g_snap = -1;
        g_testing = true, g_test = 0, g_written = false;
    }
}
