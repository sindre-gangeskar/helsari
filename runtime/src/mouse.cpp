// 1:1 mouse look. The game's look function (f_00080a78) gives its callers a look value, which they
// turn into an angle as value * speed * dt, with no acceleration at that point. So value =
// counts * scale / dt makes each mouse count one fixed angle at any frame rate, and skips the
// stick's dead zone and response curve. The stick still works: the mouse adds to its value.
#include "runtime.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" void f_00080a78_guest(PPU_CTX); // look input: (this, float *outY, float *outX, settings)

void gcm_pc_command(const std::string &cmd); // hle_gcm.cpp
std::atomic<uint32_t> g_look_entity{0}, g_look_controller{0}; // read by cam_scan.cpp

namespace
{

    constexpr uint32_t FRAME_TIME_EA = 0xf2a1d4; // the game's frame time, read by the look code's callers
    constexpr uint32_t PROFILE_PTR_EA = 0x117e740 + 576;
    constexpr uint32_t PROFILE_INVERT_Y = 91, PROFILE_INVERT_X = 90;
    constexpr float FULL_STICK_RAD_PER_S = 2.6f; // measured: full stick for 0.6 s turned about 1.59 rad
    constexpr float RAD_PER_COUNT = 0.001007f;      // at sensitivity 1
    constexpr float LOOK_NUDGE = 0.0005f;        // about 0.0001 rad per game tick
    constexpr float STEER_GAIN = 0.5f;           // of a real gap, per game tick, once the mouse is still

    std::atomic<int> g_dx{0}, g_dy{0};
    std::atomic<bool> g_captured{false};
    float g_sensitivity = 1.0f;
    std::atomic<long long> g_total_x{0}, g_total_y{0}; // every count since the start, for the view
    std::atomic<uint64_t> g_last_move_ns{0}, g_last_look_ns{0};
    std::atomic<bool> g_late_view{true};
    // What the weapon view follows (F3): 0 the view with easing, 1 the game camera with easing, 2 the game camera.
    std::atomic<int> g_gun_mode{0};
    std::atomic<bool> g_gun_test{false};
    std::atomic<uint32_t> g_look_weapon{0};    // the held weapon: an object of the same family as the player
    // Objects found by the F5 search that hold the same from/to matrix pair and turn with the player.
    constexpr int MAX_ATTACHED = 96;
    std::atomic<uint32_t> g_attached[MAX_ATTACHED];
    std::atomic<int> g_attached_count{0};
    std::atomic<uint32_t> g_attached_owner{0};
    // Does the object tilt with the view? Some stay level, and the next game tick would snap those
    // back if they were tilted. 0 not known yet: needs the player looking up or down to tell.
    std::atomic<int> g_attached_tilts[MAX_ATTACHED];
    // The per-frame look is for the mouse. Touching the controller hands everything back to the game.
    std::atomic<bool> g_mouse_active{false};
    bool debug_keys() { static const bool on = getenv("KZ2_MOUSE_DEBUG") != nullptr; return on; }
    bool per_frame_on() { return g_late_view.load(std::memory_order_relaxed) && g_mouse_active.load(std::memory_order_relaxed); }
    std::atomic<uint64_t> g_last_onfoot_ns{0}; // the on-foot look code ran: g_look_entity is the player
    // How far the game's heading and pitch are from the view, beyond movement still on its way.
    std::atomic<float> g_residual_yaw{0.f}, g_residual_pitch{0.f};
    long long g_tick_x[5], g_tick_y[5]; // mouse totals at the last game ticks

    float guest_float(uint32_t ea)
    {
        const uint32_t u = ld32(ea);
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
    void guest_float_add(uint32_t ea, float add)
    {
        const float f = guest_float(ea) + add;
        uint32_t u;
        memcpy(&u, &f, 4);
        st32(ea, u);
    }

    void update_capture(HWND h, bool active)
    {
        if (active)
        {
            RECT r;
            GetClientRect(h, &r);
            POINT c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            ClientToScreen(h, &c);
            RECT clip{c.x, c.y, c.x + 1, c.y + 1};
            ClipCursor(&clip);
        }
        else
        {
            ClipCursor(nullptr);
        }
        g_captured = active;
        g_dx = 0, g_dy = 0;
    }

    void guest_float_set(uint32_t ea, float value)
    {
        uint32_t u;
        memcpy(&u, &value, 4);
        st32(ea, u);
    }

    // The character's resource at [entity+196] limits how the view may turn. Raised, the view follows
    // the mouse at once. F7: LookHeadingAcceleration, LookPitchAcceleration. F8: LookHeadingSpeed,
    // LookPitchSpeed, HeadingAcceleration. Each key switches its group back to the game's own values.
    constexpr float NO_LIMIT = 10000.f;
    constexpr int PROBE_COUNTS = 300;
    struct Limit
    {
        uint32_t offset;
        int group;
        float original;
    };
    Limit g_limits[] = {{604, 0, 0}, {608, 0, 0}, {596, 1, 0}, {600, 1, 0}, {636, 1, 0}};
    std::atomic<bool> g_removed[2] = {{true}, {true}};

    void apply_look_limits(uint32_t entity)
    {
        static uint32_t resource;
        const uint32_t r = ld32(entity + 196);
        if (!r)
            return;
        if (r != resource)
        {
            resource = r;
            for (Limit &l : g_limits)
            {
                l.original = guest_float(r + l.offset);
                LOG("mouse", "look resource 0x%x +%u: %.3f", r, l.offset, l.original);
            }
        }
        for (const Limit &l : g_limits)
            guest_float_set(r + l.offset, g_removed[l.group].load(std::memory_order_relaxed) ? NO_LIMIT : l.original);
    }

    // The game scales the look value by the controller's factor (+784) and by the weapon's zoom factor
    // (+52 of what [r28]->virtual(+760) returns: 0.4 zoomed in), right after the look function. The
    // view has to turn by the same amount or it leaves the game's aim behind.
    std::atomic<float> g_turn_factor{1.f};

    void read_turn_factor(PPUContext *ctx)
    {
        float factor = guest_float((uint32_t)ctx->r[31] + 784);
        if (!(factor > 0.01f && factor < 4.f))
            factor = 1.f;
        const uint32_t holder = (uint32_t)ctx->r[28];
        if (holder)
        {
            PPUContext saved = *ctx;
            ctx->r[3] = holder;
            ppu_call(ctx, ld32(ld32(ld32(holder) + 760)));
            const uint32_t weapon = (uint32_t)ctx->r[3];
            *ctx = saved;
            if (weapon)
            {
                const float zoom = guest_float(weapon + 52);
                if (zoom > 0.01f && zoom <= 4.f)
                    factor *= zoom;
            }
        }
        static float logged;
        if (factor != logged)
        {
            logged = factor;
            LOG("mouse", "turn factor %.3f", factor);
        }
        g_turn_factor = factor;
    }

    // F5: the game's own turn rate. Big bursts one way and back, the view correction off, with the
    // heading of the game's camera logged every tick.
    int g_probe = -1, g_probe_pass = 0;
    float g_game_yaw;

    // What a turn changes: the player's objects before and after a burst, standing still.
    struct ProbeArea
    {
        const char *name;
        uint32_t base, size;
        std::vector<uint32_t> before;
    };
    ProbeArea g_areas[6];

    // Anything in guest memory that tilts with the view: the camera's forward direction in 3D, either
    // way round, after two vertical turns of different size. Only committed memory is read.
    std::vector<uint32_t> g_tilt_hits, g_tilters;
    std::vector<float> g_tilter_last;
    float g_cam_forward[3];

    void scan_tilters(int pass)
    {
        const float fx = g_cam_forward[0], fy = g_cam_forward[1], fz = g_cam_forward[2];
        const auto matches = [&](uint32_t ea) {
            const float a = guest_float(ea), b = guest_float(ea + 4), c = guest_float(ea + 8);
            return (fabsf(a - fx) < 0.02f && fabsf(b - fy) < 0.02f && fabsf(c - fz) < 0.02f) || (fabsf(a + fx) < 0.02f && fabsf(b + fy) < 0.02f && fabsf(c + fz) < 0.02f);
        };
        if (pass == 0)
        {
            g_tilt_hits.clear();
            for (uint64_t page = 0x00010000u; page < 0x40000000u;)
            {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery(g_mem + page, &mi, sizeof mi))
                    break;
                const uint64_t end = (uint64_t)((uint8_t *)mi.BaseAddress + mi.RegionSize - g_mem);
                if (mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE)
                    for (uint64_t ea = page; ea + 12 <= end && ea + 12 <= 0x40000000u; ea += 4)
                        if (matches((uint32_t)ea))
                            g_tilt_hits.push_back((uint32_t)ea);
                page = end > page ? end : page + 0x1000;
            }
            LOG("probe", "tilters after the first turn: %u, forward %.3f %.3f %.3f", (unsigned)g_tilt_hits.size(), fx, fy, fz);
            return;
        }
        g_tilters.clear();
        g_tilter_last.clear();
        for (const uint32_t ea : g_tilt_hits)
            if (matches(ea) && g_tilters.size() < 300)
            {
                g_tilters.push_back(ea);
                g_tilter_last.push_back(guest_float(ea + 8));
                LOG("tilter", "%x: %.4f %.4f %.4f", ea, guest_float(ea), guest_float(ea + 4), guest_float(ea + 8));
            }
        LOG("probe", "tilters after the second turn: %u, forward %.3f %.3f %.3f", (unsigned)g_tilters.size(), fx, fy, fz);
    }

    // Which tilters changed since the last frame: their rhythm shows which one the weapon follows.
    void log_tilters()
    {
        char text[256];
        int n = 0;
        for (size_t i = 0; i < g_tilters.size(); i++)
        {
            const float now = guest_float(g_tilters[i] + 8);
            if (now == g_tilter_last[i])
                continue;
            g_tilter_last[i] = now;
            n += snprintf(text + n, sizeof text - n, " %x", g_tilters[i]);
            if (n > 200)
            {
                LOG("tiltframe", "%s", text);
                n = 0;
            }
        }
        LOG("tiltframe", "frame %.1f ms:%s", host_now_ns() / 1e6, n ? text : "");
    }

    void probe_snapshot()
    {
        const uint32_t look = ld32(g_look_entity + 1168);
        g_areas[0] = {"entity", g_look_entity, 0x800, {}};
        g_areas[1] = {"controller", g_look_controller, 0x400, {}};
        g_areas[2] = {"look", look, 0x400, {}};
        const uint32_t weapon = g_look_weapon.load();
        g_areas[3] = {"weapon", weapon, 0x800, {}};
        g_areas[4] = {"weapon196", weapon ? ld32(weapon + 196) : 0, 0x200, {}};
        g_areas[5] = {"zoom", g_look_controller ? ld32(g_look_controller + 780) : 0, 0x200, {}};
        for (ProbeArea &area : g_areas)
            for (uint32_t i = 0; area.base && i < area.size; i += 4)
                area.before.push_back(ld32(area.base + i));
        if (look)
            LOG("probe", "entity %x controller %x look %x, look slot136 %x, controller slot176 %x", g_look_entity.load(), g_look_controller.load(), look,
                ld32(ld32(ld32(look) + 136)), ld32(ld32(ld32(g_look_controller) + 176)));
    }

    void probe_diff(float yaw_before)
    {
        LOG("probe", "pass %d camera yaw %.4f -> %.4f", g_probe_pass, yaw_before, g_game_yaw);
        for (ProbeArea &area : g_areas)
            for (uint32_t i = 0; i < area.before.size(); i++)
            {
                const uint32_t now = ld32(area.base + i * 4);
                if (now == area.before[i])
                    continue;
                float x, y;
                memcpy(&x, &area.before[i], 4);
                memcpy(&y, &now, 4);
                if (fabsf(x) < 1000.f && fabsf(y) < 1000.f && fabsf(y - x) > 0.002f)
                    LOG("probe", "%s+%u: %.4f -> %.4f", area.name, i * 4, x, y);
            }
    }

    void probe_step()
    {
        static float yaw_before;
        if (g_probe < 0)
            return;
        if (g_probe == 4)
        {
            yaw_before = g_game_yaw;
            probe_snapshot();
        }
        if (g_probe == 5)
            g_dx += g_probe_pass ? -500 : 400;
        if (g_probe == 12)
        {
            probe_diff(yaw_before);
            scan_tilters(g_probe_pass);
        }
        if (g_probe == 40)
            g_dx -= g_probe_pass ? -500 : 400;
        if (++g_probe > 75)
        {
            g_probe = g_probe_pass ? -1 : 0;
            g_probe_pass = !g_probe_pass;
        }
    }

} // namespace

void mouse_start(HWND h)
{
    if (const char *e = getenv("KZ2_MOUSE_SENS"))
        g_sensitivity = (float)atof(e);
    RAWINPUTDEVICE d{};
    d.usUsagePage = 1, d.usUsage = 2, d.hwndTarget = h; // generic desktop, mouse
    if (!RegisterRawInputDevices(&d, 1, sizeof d))
        LOG("mouse", "raw input not available (%lu)", GetLastError());
    update_capture(h, GetForegroundWindow() == h);
}

void mouse_inject(int counts) { g_dy += counts; } // horizontal, as if the mouse moved

bool mouse_active() { return g_captured.load(std::memory_order_relaxed); }

// Window thread: true if the message is handled here.
bool mouse_message(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *result)
{
    switch (m)
    {
    case WM_KEYDOWN:
        if (w == VK_F9 && g_look_entity)
        {
            // Zoom offset: F9 when the aim is off, F9 again after a shot or a step has corrected it.
            static bool armed;
            armed = !armed;
            LOG("probe", "F9 %s", armed ? "while off" : "after the correction");
            if (armed)
                probe_snapshot();
            else
                probe_diff(g_game_yaw);
        }
        if (w == VK_F2 && debug_keys())
        {
            g_gun_test = !g_gun_test;
            LOG("mouse", "F2 weapon matrix test: %s", g_gun_test ? "on" : "off");
        }
        if (w == VK_F3 && debug_keys())
        {
            g_gun_mode = (g_gun_mode + 1) % 3;
            static const char *const names[] = {"view, eased", "game camera, eased", "game camera"};
            LOG("mouse", "F3 weapon view follows: %s", names[g_gun_mode.load()]);
        }
        if (w == VK_F4)
        {
            g_late_view = !g_late_view;
            LOG("mouse", "F4 view between ticks: %s", g_late_view ? "on" : "off");
        }
        if (w == VK_F5 && g_probe < 0 && debug_keys())
            g_probe = 0;
        if (w == VK_F7 || w == VK_F8)
        {
            const int group = w == VK_F7 ? 0 : 1;
            const bool removed = !g_removed[group];
            g_removed[group] = removed;
            LOG("mouse", "%s: %s", group ? "F8 speed caps" : "F7 look acceleration", removed ? "removed" : "the game's own");
        }
        return false;
    case WM_INPUT:
    {
        RAWINPUT ri;
        UINT size = sizeof ri;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && g_captured)
        {
            g_dx -= ri.data.mouse.lLastY; // Correct mouse axes by flipping the x and y values with their opposites
            g_dy += ri.data.mouse.lLastX;
            g_total_x += ri.data.mouse.lLastX;
            g_total_y += ri.data.mouse.lLastY;
            if (ri.data.mouse.lLastX || ri.data.mouse.lLastY)
            {
                g_last_move_ns = host_now_ns();
                g_mouse_active = true;
            }
        }
        return false; // DefWindowProc cleans up after WM_INPUT
    }
    case WM_ACTIVATE:
        update_capture(h, LOWORD(w) != WA_INACTIVE);
        return false;
    case WM_SIZE:
    case WM_MOVE:
        if (g_captured)
            update_capture(h, true);
        return false;
    case WM_SETCURSOR:
        if (g_captured && LOWORD(l) == HTCLIENT)
        {
            SetCursor(nullptr);
            *result = TRUE;
            return true;
        }
        return false;
    }
    return false;
}

namespace
{
    // Finds the objects attached to the player: they hold a from/to pair of matrices (+64 and +128)
    // facing the way the player faces, or a quarter or half turn from it. Something standing around
    // can match by chance, so a match only counts once it still matches after the player has turned.
    bool faces_player(uint32_t row, float fx, float fy, float tolerance)
    {
        const float a = guest_float(row), b = guest_float(row + 4), z = guest_float(row + 8);
        const float flat = sqrtf(a * a + b * b);
        if (!(flat > 0.3f && flat < 1.1f && fabsf(z) < 1.05f))
            return false;
        const float along = fabsf(a * fx + b * fy) / flat, across = fabsf(a * fy - b * fx) / flat;
        return (along > across ? across : along) < tolerance;
    }

    bool is_pair_object(uint32_t base, float fx, float fy, float tolerance)
    {
        return faces_player(base + 64, fx, fy, tolerance) && faces_player(base + 80, fx, fy, tolerance) && faces_player(base + 128, fx, fy, tolerance) && faces_player(base + 144, fx, fy, tolerance);
    }

    bool settled(uint64_t now)
    {
        return now - g_last_move_ns.load(std::memory_order_relaxed) > 300000000ull && now - g_last_onfoot_ns.load(std::memory_order_relaxed) < 200000000ull;
    }

    void discovery_thread()
    {
        std::vector<uint32_t> pending;
        float scan_yaw = 0.f;
        uint32_t scanned_owner = 0, scanned_weapon = 0;
        uint64_t scanned_at = 0;
        for (;;)
        {
            Sleep(100);
            const uint32_t e = g_look_entity.load(), w = g_look_weapon.load(), ctl = g_look_controller.load();
            const uint64_t now = host_now_ns();
            if (!e || !ctl || !g_mouse_active.load() || !settled(now))
                continue;
            if (g_attached_owner.load() != e)
            {
                g_attached_count = 0;
                g_attached_owner = e;
            }
            const float fx = guest_float(e + 80), fy = guest_float(e + 84), yaw = atan2f(fy, fx);
            const float player_pitch = guest_float(ctl + 120);
            if (fabsf(player_pitch) > 0.1f)
                for (int i = 0, n = g_attached_count.load(); i < n; i++)
                    if (!g_attached_tilts[i].load())
                    {
                        const uint32_t base = g_attached[i].load();
                        const bool tilts = fabsf(guest_float(base + 88) - sinf(player_pitch)) < 0.05f;
                        g_attached_tilts[i] = tilts ? 1 : -1;
                        LOG("mouse", "attached object %x %s", base, tilts ? "tilts" : "stays level");
                    }
            if (!pending.empty() && fabsf(remainderf(yaw - scan_yaw, 6.28318531f)) > 0.4f)
            {
                int count = g_attached_count.load();
                for (const uint32_t base : pending)
                {
                    bool known = false;
                    for (int i = 0; i < count; i++)
                        known = known || g_attached[i].load() == base;
                    if (!known && count < MAX_ATTACHED && is_pair_object(base, fx, fy, 0.02f))
                    {
                        g_attached_tilts[count] = 0;
                        g_attached[count] = base;
                        g_attached_count = ++count;
                        LOG("mouse", "attached object %x, class %x", base, ld32(base));
                    }
                }
                pending.clear();
            }
            if (e == scanned_owner && w == scanned_weapon && now - scanned_at < 30000000000ull)
                continue;
            pending.clear();
            for (uint64_t page = 0x30000000u; page < 0x40000000u;)
            {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery(g_mem + page, &mi, sizeof mi))
                    break;
                const uint64_t end = (uint64_t)((uint8_t *)mi.BaseAddress + mi.RegionSize - g_mem);
                if (mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE)
                    for (uint64_t row = page; row + 96 <= end && row + 96 <= 0x40000000u; row += 16)
                    {
                        const uint32_t base = (uint32_t)row - 64;
                        const bool own = (base >= e - 64 && base < e + 0x800) || (w && base >= w - 64 && base < w + 0x800) || (base >= ctl - 64 && base < ctl + 0x400);
                        if (!own && is_pair_object(base, fx, fy, 0.02f))
                            pending.push_back(base);
                    }
                page = end > page ? end : page + 0x1000;
            }
            scanned_owner = e, scanned_weapon = w, scanned_at = host_now_ns(), scan_yaw = yaw;
            LOG("mouse", "attached object search: %u candidates, %d attached", (unsigned)pending.size(), g_attached_count.load());
        }
    }
} // namespace

// Who runs the game tick: the guest return addresses from here to the top of the stack, and the
// time between ticks. Each frame holds the caller's stack pointer at +0 and its return address at +16.
static void log_tick_chain(PPUContext *ctx)
{
    static int samples;
    static uint64_t last;
    const uint64_t now = host_now_ns();
    if (samples < 3 && last && now - last < 1000000000ull)
    {
        samples++;
        char text[512];
        int n = snprintf(text, sizeof text, "%.1f ms:", (now - last) / 1e6);
        uint32_t sp = (uint32_t)ctx->r[1];
        for (int i = 0; i < 28 && n < 480; i++)
        {
            const uint32_t next = (uint32_t)ld64(sp);
            if (next <= sp || next - sp > 0x100000)
                break;
            n += snprintf(text + n, sizeof text - n, " %x", (uint32_t)ld64(next + 16));
            sp = next;
        }
        LOG("tick", "%s", text);
    }
    last = now;
}

extern "C" void f_00080a78(PPU_CTX)
{
    const uint32_t out_y = (uint32_t)ctx->r[4], out_x = (uint32_t)ctx->r[5];
    const uint32_t lr = (uint32_t)ctx->lr;
    g_last_look_ns = host_now_ns();
    if (lr == 0x16e794) // the on-foot caller: its registers still hold the player and its controller
    {
        g_look_entity = (uint32_t)ctx->r[30];
        g_look_controller = (uint32_t)ctx->r[31];
        g_last_onfoot_ns = host_now_ns();
        g_look_weapon = (uint32_t)ctx->r[28];
        static const bool discovery_started = [] {
            std::thread(discovery_thread).detach();
            return true;
        }();
        (void)discovery_started;
        log_tick_chain(ctx);
        read_turn_factor(ctx);
        apply_look_limits((uint32_t)ctx->r[30]);
        probe_step();
    }
    f_00080a78_guest(ctx);
    { // Which callers run, to see that the mouse goes to the right one.
        static uint32_t seen[8];
        for (uint32_t &s : seen)
        {
            if (s == lr)
                break;
            if (!s)
            {
                s = lr;
                LOG("mouse", "look function called from 0x%x", lr);
                break;
            }
        }
    }
    const int dx = g_dx.exchange(0), dy = g_dy.exchange(0);
    for (int i = 4; i > 0; i--)
        g_tick_x[i] = g_tick_x[i - 1], g_tick_y[i] = g_tick_y[i - 1];
    g_tick_x[0] = g_total_x.load(std::memory_order_relaxed);
    g_tick_y[0] = g_total_y.load(std::memory_order_relaxed);
    const float rad_per_count = RAD_PER_COUNT * g_sensitivity * g_turn_factor.load(std::memory_order_relaxed);
    // Steer the game toward the view: whatever its real scale is (limits, zoom), this closes the gap.
    const float steer_h = -g_residual_yaw.load(std::memory_order_relaxed) * STEER_GAIN / rad_per_count;
    const float steer_v = g_residual_pitch.load(std::memory_order_relaxed) * STEER_GAIN / rad_per_count;
    if (!dx && !dy && !steer_h && !steer_v)
        return;
    const float dt = guest_float(FRAME_TIME_EA);
    if (dt < 0.0001f || dt > 1.f)
        return;
    const float scale = RAD_PER_COUNT * g_sensitivity / (FULL_STICK_RAD_PER_S * dt);
    float x = dx * scale, y = dy * scale;
    float flip_x = 1.f, flip_y = 1.f;
    if (const uint32_t profile = ld32(PROFILE_PTR_EA))
    {
        if (ld8(profile + PROFILE_INVERT_X))
            flip_x = -1.f;
        if (ld8(profile + PROFILE_INVERT_Y))
            flip_y = -1.f;
    }
    x = x * flip_x + steer_v * scale;
    y = y * flip_y + steer_h * scale;
    // On foot the player's rotation is written every rendered frame (rotate_player), not by the tick.
    // The look code skips some of its work while the input is zero, so a mouse move still gives it a
    // value, far too small to turn anything by itself.
    if (lr == 0x16e794 && g_probe < 0 && per_frame_on())
    {
        x = dx ? copysignf(LOOK_NUDGE, x) : 0.f;
        y = dy ? copysignf(LOOK_NUDGE, y) : 0.f;
    }
    guest_float_add(out_x, x);
    guest_float_add(out_y, y);
}

// The view between game ticks. Game logic runs at a fixed 15 Hz and the camera is interpolated
// between ticks, so input alone reaches the screen late and blended. Each rendered frame this
// works out where the mouse says the view should be, compares that with the interpolated camera,
// and rotates the camera by the difference. Once the mouse rests the difference fades away, which
// hands the view back to the game (recoil, scripted cameras). F4 switches this off to compare.
namespace
{
    constexpr float RAD_PER_COUNT_VIEW = RAD_PER_COUNT;   // a burst of 300 counts measured 0.30 rad
    constexpr float MAX_OFFSET = 1.2f;                    // a fast flick within the two tick delay
    constexpr float REST_FADE = 0.8f;                     // per frame, once the mouse rests
    constexpr float MAX_PITCH = 1.5f;
    constexpr float MAX_GAP = 0.3f;                       // bigger than this is the game turning the player
    constexpr uint64_t VIEW_HOLD_NS = 200000000ull;       // two game ticks and a bit
    constexpr float GUN_MAX = 0.15f;                      // how far the weapon may trail, in radians
    constexpr uint64_t REST_NS = 250000000ull;            // the game has caught up by then
    constexpr uint32_t VIEW_FIRST = 0x26c8f8, VIEW_NEAR = 0x26c388, VIEW_MAIN = 0x271a8c;

    float wrap_angle(float a)
    {
        while (a > 3.14159265f)
            a -= 6.28318531f;
        while (a < -3.14159265f)
            a += 6.28318531f;
        return a;
    }

    void rotate_view_vector(uint32_t ea, float yaw, float pitch, float rx, float ry)
    {
        float x = guest_float(ea), y = guest_float(ea + 4), z = guest_float(ea + 8);
        // Pitch: about the camera's right axis (rx, ry, 0).
        const float cp = cosf(pitch), sp = sinf(pitch), d = rx * x + ry * y;
        const float px = x * cp + (ry * z) * sp + rx * d * (1 - cp);
        const float py = y * cp - (rx * z) * sp + ry * d * (1 - cp);
        const float pz = z * cp + (rx * y - ry * x) * sp;
        // Yaw: about world up.
        const float cy = cosf(yaw), sy = sinf(yaw);
        guest_float_set(ea, px * cy - py * sy);
        guest_float_set(ea + 4, px * sy + py * cy);
        guest_float_set(ea + 8, pz);
    }

    void rotate_xy(uint32_t ea, float c, float s)
    {
        const float x = guest_float(ea), y = guest_float(ea + 4);
        guest_float_set(ea, x * c - y * s);
        guest_float_set(ea + 4, x * s + y * c);
    }

    // The player's rotation, measured: entity+64 right and +80 forward, with copies at +128, +464 and
    // +784 and at controller+88, plus points ahead of the player that turn with it.
    void rotate_player(float yaw)
    {
        const uint32_t e = g_look_entity.load(std::memory_order_relaxed), ctl = g_look_controller.load(std::memory_order_relaxed);
        if (!e || !ctl || !yaw)
            return;
        const float c = cosf(yaw), s = sinf(yaw);
        for (uint32_t m : {64u, 128u, 464u, 784u})
        {
            rotate_xy(e + m, c, s);
            rotate_xy(e + m + 16, c, s);
        }
        rotate_xy(ctl + 88, c, s);
        // The held weapon and the objects attached to the player keep a from/to pair of matrices (+64
        // and +128), one game tick apart. The mouse turn goes on top of both, so the game's own values
        // for them still arrive every tick.
        const float ex = guest_float(e + 112), ey = guest_float(e + 116);
        const auto rotate_object = [&](uint32_t object) {
            for (uint32_t m : {64u, 128u})
            {
                rotate_xy(object + m, c, s);
                rotate_xy(object + m + 16, c, s);
                rotate_xy(object + m + 32, c, s);
                const float x = guest_float(object + m + 48) - ex, y = guest_float(object + m + 52) - ey;
                if (x * x + y * y < 1.f)
                {
                    guest_float_set(object + m + 48, ex + x * c - y * s);
                    guest_float_set(object + m + 52, ey + x * s + y * c);
                }
            }
        };
        if (const uint32_t weapon = g_look_weapon.load(std::memory_order_relaxed))
            rotate_object(weapon);
        if (g_attached_owner.load(std::memory_order_relaxed) != e)
            g_attached_count = 0;
        const float fx = guest_float(e + 80), fy = guest_float(e + 84);
        for (int i = 0, n = g_attached_count.load(std::memory_order_relaxed); i < n; i++)
        {
            const uint32_t object = g_attached[i].load(std::memory_order_relaxed);
            if (is_pair_object(object, fx, fy, 0.5f)) // no longer attached, or freed: leave it alone
                rotate_object(object);
        }
        const float px = guest_float(e + 832), py = guest_float(e + 836);
        const auto rotate_point = [&](uint32_t ea) {
            const float x = guest_float(ea) - px, y = guest_float(ea + 4) - py;
            guest_float_set(ea, px + x * c - y * s);
            guest_float_set(ea + 4, py + x * s + y * c);
        };
        rotate_point(e + 348);
        rotate_point(e + 372);
        rotate_point(ctl + 300);
    }

    float target_yaw, target_pitch; // where the mouse says the view points

    // The vertical turn goes onto the same matrices, about the camera. The next game tick replaces
    // them with values that already include it, so nothing adds up.
    void pitch_object(uint32_t object, float turn, float rx, float ry, float eye_x, float eye_y, float eye_z)
    {
        for (uint32_t m : {64u, 128u})
        {
            rotate_view_vector(object + m, 0.f, turn, rx, ry);
            rotate_view_vector(object + m + 16, 0.f, turn, rx, ry);
            rotate_view_vector(object + m + 32, 0.f, turn, rx, ry);
            const uint32_t at = object + m + 48;
            const float x = guest_float(at) - eye_x, y = guest_float(at + 4) - eye_y, z = guest_float(at + 8) - eye_z;
            if (x * x + y * y + z * z > 4.f)
                continue;
            const float c = cosf(turn), s = sinf(turn), d = rx * x + ry * y;
            guest_float_set(at, eye_x + x * c + ry * z * s + rx * d * (1 - c));
            guest_float_set(at + 4, eye_y + y * c - rx * z * s + ry * d * (1 - c));
            guest_float_set(at + 8, eye_z + z * c + (rx * y - ry * x) * s);
        }
    }

    void pitch_objects(float turn, uint32_t camera)
    {
        const uint32_t e = g_look_entity.load(std::memory_order_relaxed), weapon = g_look_weapon.load(std::memory_order_relaxed);
        if (!e || !turn)
            return;
        const float fx = guest_float(e + 80), fy = guest_float(e + 84);
        const float eye_x = guest_float(camera + 4), eye_y = guest_float(camera + 8), eye_z = guest_float(camera + 12);
        if (weapon)
            pitch_object(weapon, turn, fy, -fx, eye_x, eye_y, eye_z);
        for (int i = 0, n = g_attached_count.load(std::memory_order_relaxed); i < n; i++)
        {
            const uint32_t object = g_attached[i].load(std::memory_order_relaxed);
            if (g_attached_tilts[i].load(std::memory_order_relaxed) > 0 && is_pair_object(object, fx, fy, 0.5f))
                pitch_object(object, turn, fy, -fx, eye_x, eye_y, eye_z);
        }
    }

    // The player's pitch: controller+120, limited by entity+420 and +424 as the look code does.
    // Returns how far it really turned, so the view stops at the same limit.
    float pitch_player(float turn)
    {
        const uint32_t e = g_look_entity.load(std::memory_order_relaxed), ctl = g_look_controller.load(std::memory_order_relaxed);
        if (!e || !ctl)
            return turn;
        const float now = guest_float(ctl + 120), low = guest_float(e + 420), high = guest_float(e + 424);
        float next = now + turn;
        if (low < high)
            next = next < low ? low : next > high ? high : next;
        guest_float_set(ctl + 120, next);
        return next - now;
    }

    // The view that draws the weapon follows the game camera with a little delay, so the weapon
    // trails a turn and settles smoothly. Barely any delay while zoomed: the sights have to stay true.
    void gun_view(uint32_t in)
    {
        static float ease_yaw, ease_pitch;
        static uint64_t last;
        static const float tau_ms = [] {
            const char *e = getenv("KZ2_GUN_EASE_MS");
            return e ? (float)atof(e) : 0.f;
        }();
        const uint64_t now = host_now_ns();
        const float ax = guest_float(in + 16), ay = guest_float(in + 20), az = guest_float(in + 24);
        const float len = sqrtf(ax * ax + ay * ay);
        const float yaw = atan2f(ay, ax), pitch = asinf(az < -1.f ? -1.f : az > 1.f ? 1.f : az);
        const float dt_ms = (float)(now - last) / 1e6f;
        const bool fresh = !last || dt_ms > 200.f || now - g_last_onfoot_ns.load(std::memory_order_relaxed) > 200000000ull;
        last = now;
        const int mode = g_gun_mode.load(std::memory_order_relaxed);
        if (fresh || mode == 2 || len < 0.001f)
        {
            ease_yaw = yaw, ease_pitch = pitch;
            return;
        }
        const float tau = tau_ms;
        const float blend = tau > 0.f ? 1.f - expf(-dt_ms / tau) : 1.f;
        // The weapon moves every frame with the view, so its view follows that one (mode 0).
        const float source_yaw = mode == 0 ? target_yaw : yaw, source_pitch = mode == 0 ? target_pitch : pitch;
        float trail_yaw = wrap_angle(ease_yaw + wrap_angle(source_yaw - ease_yaw) * blend - source_yaw);
        float trail_pitch = ease_pitch + (source_pitch - ease_pitch) * blend - source_pitch;
        trail_yaw = trail_yaw > GUN_MAX ? GUN_MAX : trail_yaw < -GUN_MAX ? -GUN_MAX : trail_yaw;
        trail_pitch = trail_pitch > GUN_MAX ? GUN_MAX : trail_pitch < -GUN_MAX ? -GUN_MAX : trail_pitch;
        const float off_yaw = wrap_angle(source_yaw + trail_yaw - yaw);
        const float off_pitch = source_pitch + trail_pitch - pitch;
        ease_yaw = wrap_angle(yaw + off_yaw), ease_pitch = pitch + off_pitch;
        rotate_view_vector(in + 16, off_yaw, off_pitch, ay / len, -ax / len);
        rotate_view_vector(in + 28, off_yaw, off_pitch, ay / len, -ax / len);
    }

    // F2: does the renderer use the weapon's matrices? Turns them well away from the player, again
    // whenever a game tick has rewritten them. The weapon jumps aside if it does.

    void gun_test()
    {
        static float seen[2];
        const uint32_t weapon = g_look_weapon.load(std::memory_order_relaxed);
        if (!weapon || !g_gun_test.load(std::memory_order_relaxed))
            return;
        const float c = cosf(0.5f), s = sinf(0.5f);
        const uint32_t pair[2] = {64u, 128u};
        for (int i = 0; i < 2; i++)
        {
            const uint32_t m = weapon + pair[i];
            if (guest_float(m) == seen[i])
                continue;
            rotate_xy(m, c, s);
            rotate_xy(m + 16, c, s);
            rotate_xy(m + 32, c, s);
            seen[i] = guest_float(m);
        }
    }

    void late_view(uint32_t in, uint32_t lr)
    {
        static float off_yaw, off_pitch;
        static bool mouse_turned;
        static long long last_x, last_y;
        static uint64_t computed;
        const uint64_t now = host_now_ns();
        const float ax = guest_float(in + 16), ay = guest_float(in + 20), az = guest_float(in + 24);
        // No look calls recently: a menu or cutscene. The camera is the game's alone.
        if (now - g_last_look_ns.load(std::memory_order_relaxed) > 200000000ull)
        {
            off_yaw = off_pitch = 0;
            computed = 0;
            return;
        }
        const float yaw = atan2f(ay, ax), pitch = asinf(az < -1.f ? -1.f : az > 1.f ? 1.f : az);
        if (lr == VIEW_FIRST || now - computed > 50000000ull)
        {
            const float k = RAD_PER_COUNT_VIEW * g_sensitivity * g_turn_factor.load(std::memory_order_relaxed);
            const long long total_x = g_total_x.load(std::memory_order_relaxed), total_y = g_total_y.load(std::memory_order_relaxed);
            uint32_t profile = ld32(PROFILE_PTR_EA);
            const float sign_y = profile && ld8(profile + PROFILE_INVERT_X) ? -1.f : 1.f;
            const float sign_x = profile && ld8(profile + PROFILE_INVERT_Y) ? -1.f : 1.f;
            if (!computed || now - g_last_move_ns.load(std::memory_order_relaxed) > REST_NS)
            {
                // The mouse has stopped. Whatever gap built up during the turn is closed once by moving
                // the game to the view, not the view to the game, and the view holds while the game's
                // camera arrives. Only then does the view go back to following the game (recoil,
                // scripted cameras), so a bigger gap than a turn can leave is the game's own doing.
                static uint64_t closed_at;
                const uint32_t e = g_look_entity.load(std::memory_order_relaxed), ctl = g_look_controller.load(std::memory_order_relaxed);
                if (mouse_turned && computed && e && ctl && now - g_last_onfoot_ns.load(std::memory_order_relaxed) < 200000000ull)
                {
                    const float gap_yaw = wrap_angle(target_yaw - atan2f(guest_float(e + 84), guest_float(e + 80)));
                    const float gap_pitch = target_pitch - guest_float(ctl + 120);
                    if (fabsf(gap_yaw) < MAX_GAP)
                        rotate_player(gap_yaw);
                    if (fabsf(gap_pitch) < MAX_GAP)
                        pitch_objects(pitch_player(gap_pitch), in);
                    LOG("mouse", "stopped: closed a gap of yaw %.4f pitch %.4f", gap_yaw, gap_pitch);
                    closed_at = now;
                }
                mouse_turned = false;
                if (closed_at && now - closed_at < VIEW_HOLD_NS)
                {
                    off_yaw = wrap_angle(target_yaw - yaw);
                    off_pitch = target_pitch - pitch;
                }
                else
                {
                    off_yaw = computed ? off_yaw * REST_FADE : 0.f;
                    off_pitch = computed ? off_pitch * REST_FADE : 0.f;
                    target_yaw = yaw + off_yaw;
                    target_pitch = pitch + off_pitch;
                }
            }
            else
            {
                // Built up frame by frame, so a change of zoom cannot make the view jump.
                mouse_turned = true;
                const float turn = -sign_x * k * (float)(total_x - last_x);
                target_yaw = wrap_angle(target_yaw + turn);
                if (now - g_last_onfoot_ns.load(std::memory_order_relaxed) < 200000000ull)
                    rotate_player(turn);
                float turn_pitch = -sign_y * k * (float)(total_y - last_y);
                if (now - g_last_onfoot_ns.load(std::memory_order_relaxed) < 200000000ull)
                {
                    turn_pitch = pitch_player(turn_pitch);
                    pitch_objects(turn_pitch, in);
                }
                target_pitch += turn_pitch;
                off_yaw = wrap_angle(target_yaw - yaw);
                off_pitch = target_pitch - pitch;
                off_yaw = off_yaw > MAX_OFFSET ? MAX_OFFSET : off_yaw < -MAX_OFFSET ? -MAX_OFFSET : off_yaw;
                off_pitch = off_pitch > MAX_OFFSET ? MAX_OFFSET : off_pitch < -MAX_OFFSET ? -MAX_OFFSET : off_pitch;
                if (pitch + off_pitch > MAX_PITCH)
                    off_pitch = MAX_PITCH - pitch;
                if (pitch + off_pitch < -MAX_PITCH)
                    off_pitch = -MAX_PITCH - pitch;
                target_yaw = wrap_angle(yaw + off_yaw); // a limit stays a limit
                target_pitch = pitch + off_pitch;
            }
            last_x = total_x, last_y = total_y;
            // While the mouse moves the game is expected to trail the view, so the difference says
            // nothing. Only once it has been still for two ticks is a difference a real gap.
            const bool resting = now - g_last_move_ns.load(std::memory_order_relaxed) > REST_NS;
            const bool calm = total_x == g_tick_x[2] && total_y == g_tick_y[2];
            g_residual_yaw = resting || !calm ? 0.f : off_yaw;
            g_residual_pitch = resting || !calm ? 0.f : off_pitch;
            static uint64_t logged;
            if (now - logged > 1000000000ull && !resting)
            {
                logged = now;
                const uint32_t e = g_look_entity.load(std::memory_order_relaxed);
                const uint32_t ctl = g_look_controller.load(std::memory_order_relaxed);
                LOG("mouse", "yaw: view target %.3f, game camera %.3f, player %.3f; pitch: view target %.3f, game camera %.3f, player %.3f (limits %.3f %.3f)", target_yaw, yaw,
                    e ? atan2f(guest_float(e + 84), guest_float(e + 80)) : 0.f, target_pitch, pitch, ctl ? guest_float(ctl + 120) : 0.f, e ? guest_float(e + 420) : 0.f, e ? guest_float(e + 424) : 0.f);
            }
            computed = now;
        }
        if (!off_yaw && !off_pitch)
            return;
        const float len = sqrtf(ax * ax + ay * ay);
        if (len < 0.001f)
            return;
        const float rx = ay / len, ry = -ax / len;   // forward x up, in the horizontal plane
        rotate_view_vector(in + 16, off_yaw, off_pitch, rx, ry);
        rotate_view_vector(in + 28, off_yaw, off_pitch, rx, ry);
    }
} // namespace

// Builds view data from a camera struct (position +4, forward +16, up +28), per rendered frame.
extern "C" void f_006623d8_guest(PPU_CTX);
extern "C" void f_006623d8(PPU_CTX)
{
    const uint32_t lr = (uint32_t)ctx->lr;
    if (lr == VIEW_MAIN)
        g_game_yaw = atan2f(guest_float((uint32_t)ctx->r[4] + 20), guest_float((uint32_t)ctx->r[4] + 16));
    if (g_probe >= 0)
        g_residual_yaw = 0.f, g_residual_pitch = 0.f;
    if (g_probe < 0 && per_frame_on() && (lr == VIEW_FIRST || lr == VIEW_MAIN)) // not VIEW_NEAR: it draws the weapon, which stays with the game's camera
        late_view((uint32_t)ctx->r[4], lr);
    if (lr == VIEW_FIRST)
        gun_test();
    if (lr == VIEW_MAIN && g_probe >= 0)
        for (int i = 0; i < 3; i++)
            g_cam_forward[i] = guest_float((uint32_t)ctx->r[4] + 16 + i * 4);
    if (lr == VIEW_NEAR && g_probe_pass == 1 && g_probe >= 39 && g_probe <= 48)
        log_tilters();
    if (g_probe < 0 && per_frame_on() && lr == VIEW_NEAR)
        gun_view((uint32_t)ctx->r[4]);
    f_006623d8_guest(ctx);
}

// The controller was used: hand the look back to the game until the mouse moves again.
void mouse_note_controller()
{
    g_mouse_active = false;
}
