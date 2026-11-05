// 1:1 mouse look. Game logic runs at a fixed 15 Hz with an interpolated camera, so input through the
// look code reaches the screen late and blended. With the mouse, on foot, the player's rotation
// (entity matrices, controller pitch) is written every rendered frame instead, and the view is
// corrected to match. Using a controller hands everything back to the game until the mouse moves.
// The viewmodel has its own part of this in rsx/d3d.cpp. KZ2_MOUSE_OFF: the hooks only pass through.
#include "keybinds.h"

#include "runtime.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

extern "C" void f_00080a78_guest(PPU_CTX); // look input: (this, float *outY, float *outX, settings)
extern "C" void f_006623d8_guest(PPU_CTX); // view data from a camera struct: position +4, forward +16, up +28

namespace
{
    constexpr uint32_t TICK_TIME_EA = 0xf2a1d4; // the length of a game tick, read by the look code's callers
    constexpr uint32_t PROFILE_PTR_EA = 0x117e740 + 576;
    constexpr uint32_t PROFILE_INVERT_Y = 91, PROFILE_INVERT_X = 90;
    constexpr float FULL_STICK_RAD_PER_S = 2.6f; // measured: full stick for 0.6 s turned about 1.59 rad
    constexpr float RAD_PER_COUNT = 0.001007f;   // at sensitivity 1
    constexpr float LOOK_NUDGE = 0.0005f;        // about 0.0001 rad per game tick
    constexpr float STEER_GAIN = 0.5f;           // of a real gap, per game tick, once the mouse is still
    constexpr uint32_t LOOK_ON_FOOT = 0x16e794;  // return address of the on-foot look code's call
    constexpr uint32_t VIEW_FIRST = 0x26c8f8, VIEW_NEAR = 0x26c388, VIEW_MAIN = 0x271a8c;
    constexpr float MAX_OFFSET = 1.2f;           // a fast flick within the two tick delay
    constexpr float REST_FADE = 0.8f;            // per frame, once the mouse rests
    constexpr float MAX_PITCH = 1.5f;
    constexpr float MAX_GAP = 0.3f;              // bigger than this is the game turning the player
    constexpr float WEAPON_VIEW_MAX = 0.05f;     // how far the weapon view may lag, on each axis
    constexpr uint64_t VIEW_HOLD_NS = 200000000ull; // two game ticks and a bit
    constexpr uint64_t REST_NS = 250000000ull;   // the game has caught up by then
    constexpr uint64_t ON_FOOT_NS = 200000000ull; // the on-foot look code ran this recently
    constexpr uint64_t TURNING_NS = 60000000ull; // a few frames: the mouse does not report on every one

    const bool g_mouse_off = getenv("KZ2_MOUSE_OFF") != nullptr;
    std::atomic<int> g_dx{0}, g_dy{0};
    std::atomic<bool> g_captured{false};
    float g_sensitivity = 1.0f;
    std::atomic<long long> g_total_x{0}, g_total_y{0}; // every count since the start, for the view
    std::atomic<uint64_t> g_last_move_ns{0}, g_last_look_ns{0}, g_last_onfoot_ns{0};
    std::atomic<bool> g_mouse_active{true}; // PC input; hle_io turns it off when a controller is there
    std::atomic<uint32_t> g_look_entity{0}, g_look_controller{0};
    std::atomic<float> g_turn_factor{1.f};
    // How far the game's heading and pitch are from the view, once the mouse is still.
    std::atomic<float> g_residual_yaw{0.f}, g_residual_pitch{0.f};
    long long g_tick_x[3], g_tick_y[3]; // mouse totals at the last game ticks
    // The player's rotation at the last game ticks, for the weapon view.
    float g_tick_yaw[3], g_tick_pitch[3];
    uint64_t g_tick_ns[3];
    float g_target_yaw, g_target_pitch; // where the mouse says the view points

    bool on_foot(uint64_t now) { return now - g_last_onfoot_ns.load(std::memory_order_relaxed) < ON_FOOT_NS; }

    float guest_float(uint32_t ea)
    {
        const uint32_t u = ld32(ea);
        float f;
        memcpy(&f, &u, 4);
        return f;
    }

    void guest_float_set(uint32_t ea, float value)
    {
        uint32_t u;
        memcpy(&u, &value, 4);
        st32(ea, u);
    }

    void guest_float_add(uint32_t ea, float add) { guest_float_set(ea, guest_float(ea) + add); }

    float wrap_angle(float a)
    {
        while (a > 3.14159265f)
            a -= 6.28318531f;
        while (a < -3.14159265f)
            a += 6.28318531f;
        return a;
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

    // The character's resource at [entity+196] limits how the view may turn: LookHeadingAcceleration,
    // LookPitchAcceleration, LookHeadingSpeed, LookPitchSpeed, HeadingAcceleration. Raised, the view
    // follows the mouse at once.
    void apply_look_limits(uint32_t entity)
    {
        constexpr float NO_LIMIT = 10000.f;
        const uint32_t resource = ld32(entity + 196);
        if (!resource)
            return;
        for (const uint32_t offset : {604u, 608u, 596u, 600u, 636u})
            guest_float_set(resource + offset, NO_LIMIT);
    }

    // The game scales the look value by the controller's factor (+784) and by the weapon's zoom factor
    // (+52 of what [r28]->virtual(+760) returns: 0.4 zoomed in), right after the look function. The
    // view has to turn by the same amount or it leaves the game's aim behind.
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
        g_turn_factor = factor;
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
    // +784 and at controller+88. Not the points ahead of the player (entity+348, +372, controller+300):
    // rotating those went with crashes and freezes when the player dies.
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

    // The camera the viewmodel is drawn with: rebuilt from the player's rotation at the last game ticks,
    // one tick late, within a limit. The sideways lag turns about the camera's own up axis: about the
    // world's it would become a roll when looking up or down.
    void weapon_view(uint32_t in)
    {
        const uint64_t now = host_now_ns();
        const float ax = guest_float(in + 16), ay = guest_float(in + 20), az = guest_float(in + 24);
        const float len = sqrtf(ax * ax + ay * ay);
        if (!on_foot(now) || len < 0.001f || !g_tick_ns[2])
            return;
        const float yaw = atan2f(ay, ax), pitch = asinf(az < -1.f ? -1.f : az > 1.f ? 1.f : az);
        const float span = (float)(g_tick_ns[1] - g_tick_ns[2]);
        float t = span > 0.f ? (float)(now - g_tick_ns[0]) / span : 1.f;
        t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
        const float want_yaw = g_tick_yaw[2] + wrap_angle(g_tick_yaw[1] - g_tick_yaw[2]) * t;
        const float want_pitch = g_tick_pitch[2] + (g_tick_pitch[1] - g_tick_pitch[2]) * t;
        const auto limit = [](float v) { return v > WEAPON_VIEW_MAX ? WEAPON_VIEW_MAX : v < -WEAPON_VIEW_MAX ? -WEAPON_VIEW_MAX : v; };
        const float gun_yaw = limit(wrap_angle(want_yaw - yaw)), gun_pitch = limit(want_pitch - pitch);
        rotate_view_vector(in + 16, 0.f, gun_pitch, ay / len, -ax / len);
        rotate_view_vector(in + 28, 0.f, gun_pitch, ay / len, -ax / len);
        const float ux = guest_float(in + 28), uy = guest_float(in + 32), uz = guest_float(in + 36);
        const float fx = guest_float(in + 16), fy = guest_float(in + 20), fz = guest_float(in + 24);
        const float angle = gun_yaw * cosf(want_pitch), c = cosf(angle), s = sinf(angle), d = ux * fx + uy * fy + uz * fz;
        guest_float_set(in + 16, fx * c + (uy * fz - uz * fy) * s + ux * d * (1 - c));
        guest_float_set(in + 20, fy * c + (uz * fx - ux * fz) * s + uy * d * (1 - c));
        guest_float_set(in + 24, fz * c + (ux * fy - uy * fx) * s + uz * d * (1 - c));
    }

    // Each rendered frame: the mouse turns the player and the target, and the camera is rotated by
    // what its interpolation still lacks. Once the mouse rests the difference fades away, which hands
    // the view back to the game (recoil, scripted cameras).
    void late_view(uint32_t in, uint32_t lr)
    {
        static float off_yaw, off_pitch;
        static bool mouse_turned;
        static long long last_x, last_y;
        static uint64_t computed, closed_at;
        const uint64_t now = host_now_ns();
        const float ax = guest_float(in + 16), ay = guest_float(in + 20), az = guest_float(in + 24);
        // No look calls recently: a menu or cutscene. The camera is the game's alone.
        if (now - g_last_look_ns.load(std::memory_order_relaxed) > ON_FOOT_NS)
        {
            off_yaw = off_pitch = 0;
            computed = 0;
            return;
        }
        const float yaw = atan2f(ay, ax), pitch = asinf(az < -1.f ? -1.f : az > 1.f ? 1.f : az);
        if (lr == VIEW_FIRST || now - computed > 50000000ull)
        {
            const float k = RAD_PER_COUNT * g_sensitivity * g_turn_factor.load(std::memory_order_relaxed);
            const long long total_x = g_total_x.load(std::memory_order_relaxed), total_y = g_total_y.load(std::memory_order_relaxed);
            const uint32_t profile = ld32(PROFILE_PTR_EA);
            const float sign_y = profile && ld8(profile + PROFILE_INVERT_X) ? -1.f : 1.f;
            const float sign_x = profile && ld8(profile + PROFILE_INVERT_Y) ? -1.f : 1.f;
            const bool resting = now - g_last_move_ns.load(std::memory_order_relaxed) > REST_NS;
            if (!computed || resting)
            {
                // The mouse has stopped. Whatever gap built up during the turn is closed once by moving
                // the game to the view, not the view to the game, and the view holds while the game's
                // camera arrives. A bigger gap than a turn can leave is the game's own doing.
                const uint32_t e = g_look_entity.load(std::memory_order_relaxed), ctl = g_look_controller.load(std::memory_order_relaxed);
                if (mouse_turned && computed && e && ctl && on_foot(now))
                {
                    const float gap_yaw = wrap_angle(g_target_yaw - atan2f(guest_float(e + 84), guest_float(e + 80)));
                    const float gap_pitch = g_target_pitch - guest_float(ctl + 120);
                    if (fabsf(gap_yaw) < MAX_GAP)
                        rotate_player(gap_yaw);
                    if (fabsf(gap_pitch) < MAX_GAP)
                        pitch_player(gap_pitch);
                    closed_at = now;
                }
                mouse_turned = false;
                if (closed_at && now - closed_at < VIEW_HOLD_NS)
                {
                    off_yaw = wrap_angle(g_target_yaw - yaw);
                    off_pitch = g_target_pitch - pitch;
                }
                else
                {
                    off_yaw = computed ? off_yaw * REST_FADE : 0.f;
                    off_pitch = computed ? off_pitch * REST_FADE : 0.f;
                    g_target_yaw = yaw + off_yaw;
                    g_target_pitch = pitch + off_pitch;
                }
            }
            else
            {
                // Built up frame by frame, so a change of zoom cannot make the view jump.
                mouse_turned = true;
                const float turn = -sign_x * k * (float)(total_x - last_x);
                g_target_yaw = wrap_angle(g_target_yaw + turn);
                float turn_pitch = -sign_y * k * (float)(total_y - last_y);
                if (on_foot(now))
                {
                    rotate_player(turn);
                    turn_pitch = pitch_player(turn_pitch);
                }
                g_target_pitch += turn_pitch;
                off_yaw = wrap_angle(g_target_yaw - yaw);
                off_pitch = g_target_pitch - pitch;
                off_yaw = off_yaw > MAX_OFFSET ? MAX_OFFSET : off_yaw < -MAX_OFFSET ? -MAX_OFFSET : off_yaw;
                off_pitch = off_pitch > MAX_OFFSET ? MAX_OFFSET : off_pitch < -MAX_OFFSET ? -MAX_OFFSET : off_pitch;
                if (pitch + off_pitch > MAX_PITCH)
                    off_pitch = MAX_PITCH - pitch;
                if (pitch + off_pitch < -MAX_PITCH)
                    off_pitch = -MAX_PITCH - pitch;
                g_target_yaw = wrap_angle(yaw + off_yaw); // a limit stays a limit
                g_target_pitch = pitch + off_pitch;
            }
            last_x = total_x, last_y = total_y;
            // While the mouse moves the game is expected to trail the view, so the difference says
            // nothing. Only once it has been still for two ticks is a difference a real gap.
            const bool calm = total_x == g_tick_x[2] && total_y == g_tick_y[2];
            g_residual_yaw = resting || !calm ? 0.f : off_yaw;
            g_residual_pitch = resting || !calm ? 0.f : off_pitch;
            computed = now;
        }
        if (!off_yaw && !off_pitch)
            return;
        const float len = sqrtf(ax * ax + ay * ay);
        if (len < 0.001f)
            return;
        const float rx = ay / len, ry = -ax / len; // forward x up, in the horizontal plane
        rotate_view_vector(in + 16, off_yaw, off_pitch, rx, ry);
        rotate_view_vector(in + 28, off_yaw, off_pitch, rx, ry);
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

bool mouse_active() { return g_captured.load(std::memory_order_relaxed); }

// Window thread: true if the message is handled here.
bool mouse_message(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *result)
{
    keybind_message(m, w, l);
    switch (m)
    {
    case WM_INPUT:
    {
        RAWINPUT ri;
        UINT size = sizeof ri;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && g_captured)
        {
            g_dx -= ri.data.mouse.lLastY; // The look code's first output is horizontal, its second vertical
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

extern "C" void f_00080a78(PPU_CTX)
{
    if (g_mouse_off)
    {
        f_00080a78_guest(ctx);
        return;
    }
    const uint32_t out_y = (uint32_t)ctx->r[4], out_x = (uint32_t)ctx->r[5];
    const uint32_t lr = (uint32_t)ctx->lr;
    g_last_look_ns = host_now_ns();
    if (lr == LOOK_ON_FOOT) // its registers still hold the player and its controller
    {
        g_look_entity = (uint32_t)ctx->r[30];
        g_look_controller = (uint32_t)ctx->r[31];
        g_last_onfoot_ns = host_now_ns();
        for (int i = 2; i > 0; i--)
        {
            g_tick_yaw[i] = g_tick_yaw[i - 1];
            g_tick_pitch[i] = g_tick_pitch[i - 1];
            g_tick_ns[i] = g_tick_ns[i - 1];
        }
        g_tick_yaw[0] = atan2f(guest_float((uint32_t)ctx->r[30] + 84), guest_float((uint32_t)ctx->r[30] + 80));
        g_tick_pitch[0] = guest_float((uint32_t)ctx->r[31] + 120);
        g_tick_ns[0] = host_now_ns();
        read_turn_factor(ctx);
        apply_look_limits((uint32_t)ctx->r[30]);
    }
    f_00080a78_guest(ctx);
    const int dx = g_dx.exchange(0), dy = g_dy.exchange(0);
    for (int i = 2; i > 0; i--)
        g_tick_x[i] = g_tick_x[i - 1], g_tick_y[i] = g_tick_y[i - 1];
    g_tick_x[0] = g_total_x.load(std::memory_order_relaxed);
    g_tick_y[0] = g_total_y.load(std::memory_order_relaxed);
    const float rad_per_count = RAD_PER_COUNT * g_sensitivity * g_turn_factor.load(std::memory_order_relaxed);
    // Steer the game toward the view: whatever its real scale is (limits, zoom), this closes the gap.
    const float steer_h = -g_residual_yaw.load(std::memory_order_relaxed) * STEER_GAIN / rad_per_count;
    const float steer_v = g_residual_pitch.load(std::memory_order_relaxed) * STEER_GAIN / rad_per_count;
    if (!dx && !dy && !steer_h && !steer_v)
        return;
    const float dt = guest_float(TICK_TIME_EA);
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
    // On foot the rotation is written every rendered frame, not by the tick. The look code skips some
    // of its work while the input is zero, so a mouse move still gives it a value, far too small to
    // turn anything by itself.
    if (lr == LOOK_ON_FOOT && g_mouse_active.load(std::memory_order_relaxed))
    {
        x = dx ? copysignf(LOOK_NUDGE, x) : 0.f;
        y = dy ? copysignf(LOOK_NUDGE, y) : 0.f;
    }
    guest_float_add(out_x, x);
    guest_float_add(out_y, y);
}

extern "C" void f_006623d8(PPU_CTX)
{
    if (!g_mouse_off && g_mouse_active.load(std::memory_order_relaxed))
    {
        const uint32_t lr = (uint32_t)ctx->lr;
        if (lr == VIEW_FIRST || lr == VIEW_MAIN)
            late_view((uint32_t)ctx->r[4], lr);
        else if (lr == VIEW_NEAR)
            weapon_view((uint32_t)ctx->r[4]);
    }
    f_006623d8_guest(ctx);
}

// The controller was used: hand the look back to the game until the mouse moves again.
void mouse_note_controller()
{
    g_mouse_active = false;
}

// A key was pressed.
void mouse_note_pc_input()
{
    g_mouse_active = true;
}

// The look code is running: in play, not in a menu.
bool mouse_in_play()
{
    return host_now_ns() - g_last_look_ns.load(std::memory_order_relaxed) < ON_FOOT_NS;
}

// For the viewmodel in rsx/d3d.cpp and the prompts.
bool mouse_look_active()
{
    return !g_mouse_off && g_mouse_active.load(std::memory_order_relaxed);
}

bool mouse_turning()
{
    return host_now_ns() - g_last_move_ns.load(std::memory_order_relaxed) < TURNING_NS;
}
