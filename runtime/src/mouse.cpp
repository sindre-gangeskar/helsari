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
    constexpr uint64_t PLAYER_WRITE_NS = 120000000ull; // about two game ticks
    constexpr uint64_t NEW_PLAYER_NS = 1500000000ull;  // how long a respawn may correct the view
    constexpr uint64_t TURNING_NS = 60000000ull; // a few frames: the mouse does not report on every one

    const bool g_mouse_off = getenv("KZ2_MOUSE_OFF") != nullptr;
    std::atomic<int> g_dx{0}, g_dy{0};
    std::atomic<bool> g_captured{false};
    float g_sensitivity = 1.0f;
    std::atomic<long long> g_total_x{0}, g_total_y{0}; // every count since the start, for the view
    std::atomic<uint64_t> g_last_move_ns{0}, g_last_look_ns{0}, g_last_onfoot_ns{0};
    std::atomic<bool> g_mouse_active{true}; // PC input; hle_io turns it off when a controller is there
    std::atomic<uint32_t> g_look_entity{0}, g_look_controller{0};
    std::atomic<uint32_t> g_look_entity_tag{0}, g_look_controller_tag{0}; // their first words at the last tick
    std::atomic<float> g_turn_factor{1.f};
    // How far the game's heading and pitch are from the view, once the mouse is still.
    std::atomic<float> g_residual_yaw{0.f}, g_residual_pitch{0.f};
    long long g_tick_x[3], g_tick_y[3]; // mouse totals at the last game ticks
    // The player's rotation at the last game ticks, for the weapon view.
    float g_tick_yaw[3], g_tick_pitch[3];
    uint64_t g_tick_ns[3];
    float g_target_yaw, g_target_pitch; // where the mouse says the view points
    std::atomic<uint64_t> g_new_player_ns{0}; // when a tick first saw a new player, or one back on foot
    float g_pending_yaw, g_pending_pitch;     // turns made while the player could not be written
    float g_weapon_view_yaw, g_weapon_view_pitch; // the weapon_view's last turn, for captures

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
    // +784 and at controller+88, and the points ahead of the player: the weapon follows those.
    // The player is only written while it is certainly still there: a tick looked at it moments ago and
    // it still starts as it did then. It goes away at death, and its memory is used for something else.
    bool player_writable(uint32_t e, uint32_t ctl)
    {
        if (!e || !ctl || host_now_ns() - g_last_onfoot_ns.load(std::memory_order_relaxed) > PLAYER_WRITE_NS)
            return false;
        if (ld32(e) == g_look_entity_tag.load(std::memory_order_relaxed) && ld32(ctl) == g_look_controller_tag.load(std::memory_order_relaxed))
            return true;
        static int logs;
        if (logs++ < 8)
            LOG("mouse", "player %x / %x changed since the last tick: not writing it", e, ctl);
        return false;
    }

    void rotate_player(float yaw)
    {
        const uint32_t e = g_look_entity.load(std::memory_order_relaxed), ctl = g_look_controller.load(std::memory_order_relaxed);
        if (!player_writable(e, ctl))
        {
            g_pending_yaw += yaw; // the tick is late (a hit, a stutter): the turn waits for it
            return;
        }
        yaw += g_pending_yaw;
        g_pending_yaw = 0.f;
        if (!yaw)
            return;
        const float c = cosf(yaw), s = sinf(yaw);
        for (uint32_t m : {64u, 128u, 464u, 784u})
        {
            rotate_xy(e + m, c, s);
            rotate_xy(e + m + 16, c, s);
        }
        rotate_xy(ctl + 88, c, s);
        // The points ahead of the player turn about its position (entity+832), and only when they are as
        // far from it as they should be: 30 units. KZ2_NO_ROTATE_AIM=1 leaves them alone.
        static const bool rotate_aim = getenv("KZ2_NO_ROTATE_AIM") == nullptr;
        if (rotate_aim)
        {
            const float px = guest_float(e + 832), py = guest_float(e + 836);
            const auto rotate_point = [&](uint32_t ea, float reach) {
                const float x = guest_float(ea) - px, y = guest_float(ea + 4) - py, d2 = x * x + y * y;
                if (!(d2 > reach * reach * 0.25f && d2 < reach * reach * 4.f))
                    return;
                guest_float_set(ea, px + x * c - y * s);
                guest_float_set(ea + 4, py + x * s + y * c);
            };
            rotate_point(e + 348, 30.f);
            rotate_point(e + 372, 30.f);
            rotate_point(ctl + 300, 30.f);
        }
    }

    // The player's pitch: controller+120, limited by entity+420 and +424 as the look code does.
    // Returns how far it really turned, so the view stops at the same limit.
    float pitch_player(float turn)
    {
        const uint32_t e = g_look_entity.load(std::memory_order_relaxed), ctl = g_look_controller.load(std::memory_order_relaxed);
        if (!player_writable(e, ctl))
        {
            g_pending_pitch += turn;
            return turn;
        }
        const float waiting = g_pending_pitch;
        g_pending_pitch = 0.f;
        const float now = guest_float(ctl + 120), low = guest_float(e + 420), high = guest_float(e + 424);
        float next = now + turn + waiting;
        if (low < high)
            next = next < low ? low : next > high ? high : next;
        guest_float_set(ctl + 120, next);
        return next - now - waiting;
    }

    // The camera the viewmodel is drawn with: rebuilt from the player's rotation at the last game ticks,
    // one tick late, within a limit. The sideways lag turns about the camera's own up axis: about the
    // world's it would become a roll when looking up or down.
    void weapon_view(uint32_t in)
    {
        const uint64_t now = host_now_ns();
        const float ax = guest_float(in + 16), ay = guest_float(in + 20), az = guest_float(in + 24);
        const float len = sqrtf(ax * ax + ay * ay);
        g_weapon_view_yaw = g_weapon_view_pitch = 0.f;
        // Only while the mouse turns the view: at rest any difference is the game's own (recoil, a hit),
        // and correcting it takes the sights off centre. It fades in and out: switched, a slow turn makes it pop.
        static float weight;
        static uint64_t last_call;
        const float step = (float)(now - last_call) / 100000000.f;
        last_call = now;
        const float wanted = now - g_last_move_ns.load(std::memory_order_relaxed) < REST_NS ? 1.f : 0.f;
        weight += (wanted - weight) * (step > 1.f ? 1.f : step);
        if (!on_foot(now) || len < 0.001f || !g_tick_ns[2] || weight < 0.001f)
            return;
        const float yaw = atan2f(ay, ax), pitch = asinf(az < -1.f ? -1.f : az > 1.f ? 1.f : az);
        const float span = (float)(g_tick_ns[1] - g_tick_ns[2]);
        float t = span > 0.f ? (float)(now - g_tick_ns[0]) / span : 1.f;
        t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
        const float want_yaw = g_tick_yaw[2] + wrap_angle(g_tick_yaw[1] - g_tick_yaw[2]) * t;
        const float want_pitch = g_tick_pitch[2] + (g_tick_pitch[1] - g_tick_pitch[2]) * t;
        const auto limit = [](float v) { return v > WEAPON_VIEW_MAX ? WEAPON_VIEW_MAX : v < -WEAPON_VIEW_MAX ? -WEAPON_VIEW_MAX : v; };
        const float gun_yaw = limit(wrap_angle(want_yaw - yaw)) * weight, gun_pitch = limit(want_pitch - pitch) * weight;
        g_weapon_view_yaw = gun_yaw, g_weapon_view_pitch = gun_pitch;
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
        { // KZ2_LOOK_LOG: whenever the view and the player are apart, up to ten times a second.
            static const bool gap_log = getenv("KZ2_LOOK_LOG") != nullptr;
            static uint64_t last_gap_log;
            const uint32_t player = g_look_entity.load(std::memory_order_relaxed);
            if (gap_log && player && now - last_gap_log > 100000000ull)
            {
                const float heading = atan2f(guest_float(player + 84), guest_float(player + 80));
                if (fabsf(wrap_angle(g_target_yaw - heading)) > 0.2f || fabsf(wrap_angle(yaw - heading)) > 0.2f)
                {
                    last_gap_log = now;
                    LOG("mouse", "apart: target %.3f camera %.3f heading %.3f off %.3f on_foot %d look %.0f ms ago onfoot %.0f ms ago moved %.0f ms ago turned %d",
                        g_target_yaw, yaw, heading, off_yaw, (int)on_foot(now), (now - g_last_look_ns.load()) / 1e6,
                        (now - g_last_onfoot_ns.load()) / 1e6, (now - g_last_move_ns.load()) / 1e6, (int)mouse_turned);
                }
            }
        }
        if (lr == VIEW_FIRST || now - computed > 50000000ull)
        {
            const float k = RAD_PER_COUNT * g_sensitivity * g_turn_factor.load(std::memory_order_relaxed);
            const long long total_x = g_total_x.load(std::memory_order_relaxed), total_y = g_total_y.load(std::memory_order_relaxed);
            const uint32_t profile = ld32(PROFILE_PTR_EA);
            const float sign_y = profile && ld8(profile + PROFILE_INVERT_X) ? -1.f : 1.f;
            const float sign_x = profile && ld8(profile + PROFILE_INVERT_Y) ? -1.f : 1.f;
            const bool resting = now - g_last_move_ns.load(std::memory_order_relaxed) > REST_NS;
            // What turned the player since our last look at it was the game: a melee hit, a stagger. The
            // view turns with it, on top of the mouse. KZ2_NO_GAME_TURNS=1: the view ignores them.
            static const bool game_turns = getenv("KZ2_NO_GAME_TURNS") == nullptr;
            static uint32_t seen_player;
            static float seen_heading, seen_pitch;
            const uint32_t player = g_look_entity.load(std::memory_order_relaxed), controller = g_look_controller.load(std::memory_order_relaxed);
            if (game_turns && on_foot(now) && player_writable(player, controller))
            {
                if (player == seen_player)
                {
                    const float heading = atan2f(guest_float(player + 84), guest_float(player + 80));
                    g_target_yaw = wrap_angle(g_target_yaw + wrap_angle(heading - seen_heading));
                    g_target_pitch += guest_float(controller + 120) - seen_pitch;
                }
                else
                    g_target_yaw = atan2f(guest_float(player + 84), guest_float(player + 80)), g_target_pitch = pitch;
            }
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
                    // Just after a respawn the view is still the old player's: it may face the other way.
                    // Only after a tick has seen the new one: during the reload the player is not there.
                    const uint32_t player = g_look_entity.load(std::memory_order_relaxed);
                    if (now - g_new_player_ns.load(std::memory_order_relaxed) < NEW_PLAYER_NS &&
                        player_writable(player, g_look_controller.load(std::memory_order_relaxed)))
                    {
                        const float heading = atan2f(guest_float(player + 84), guest_float(player + 80));
                        if (fabsf(wrap_angle(g_target_yaw - heading)) > MAX_GAP)
                            g_target_yaw = heading;
                    }
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
            if (game_turns && on_foot(now) && player_writable(player, controller))
            {
                seen_player = player;
                seen_heading = atan2f(guest_float(player + 84), guest_float(player + 80));
                seen_pitch = guest_float(controller + 120);
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
    { // KZ2_LOOK_LOG: once a second, which callers run the look code and where the player points.
        static const bool look_log = getenv("KZ2_LOOK_LOG") != nullptr;
        static uint32_t callers[4];
        static uint64_t last_log;
        if (look_log)
        {
            for (uint32_t &c : callers)
            {
                if (c == lr)
                    break;
                if (!c)
                {
                    c = lr;
                    break;
                }
            }
            const uint64_t t = host_now_ns();
            if (t - last_log > 1000000000ull)
            {
                last_log = t;
                const uint32_t e = g_look_entity.load(), c = g_look_controller.load();
                if (e && c)
                    LOG("mouse", "look callers %x %x %x %x on_foot %d entity %x ctl %x target %.3f entity %.3f ctl %.3f aim_e %.3f %.3f aim_c %.3f moved %.1f s ago",
                        callers[0], callers[1], callers[2], callers[3], on_foot(t), e, c, g_target_yaw,
                        atan2f(guest_float(e + 84), guest_float(e + 80)), atan2f(guest_float(c + 92), guest_float(c + 88)),
                        atan2f(guest_float(e + 352), guest_float(e + 348)), atan2f(guest_float(e + 376), guest_float(e + 372)),
                        atan2f(guest_float(c + 304), guest_float(c + 300)), (t - g_last_move_ns.load()) / 1e9);
                memset(callers, 0, sizeof callers);
            }
        }
    }
    if (lr == LOOK_ON_FOOT) // its registers still hold the player and its controller
    {
        if (g_look_entity.load(std::memory_order_relaxed) != (uint32_t)ctx->r[30] ||
            host_now_ns() - g_last_onfoot_ns.load(std::memory_order_relaxed) > 500000000ull)
        {
            g_new_player_ns = host_now_ns();
            g_pending_yaw = g_pending_pitch = 0.f;
        }
        g_look_entity = (uint32_t)ctx->r[30];
        g_look_controller = (uint32_t)ctx->r[31];
        g_look_entity_tag = ld32((uint32_t)ctx->r[30]);
        g_look_controller_tag = ld32((uint32_t)ctx->r[31]);
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

float mouse_turn_factor() { return g_turn_factor.load(std::memory_order_relaxed); }
void mouse_weapon_view(float &yaw, float &pitch) { yaw = g_weapon_view_yaw, pitch = g_weapon_view_pitch; }

// For captures: where the player, and the points ahead of it, point, and how far away those are.
void mouse_log_aim()
{
    const uint32_t e = g_look_entity.load(std::memory_order_relaxed), c = g_look_controller.load(std::memory_order_relaxed);
    if (!e || !c)
        return;
    const float px = guest_float(e + 832), py = guest_float(e + 836);
    const auto angle = [&](uint32_t ea) { return atan2f(guest_float(ea + 4) - py, guest_float(ea) - px); };
    const auto reach = [&](uint32_t ea) { return hypotf(guest_float(ea + 4) - py, guest_float(ea) - px); };
    LOG("gun", "aim target %.3f heading %.3f ctl %.3f | points %.3f (%.1f) %.3f (%.1f) %.3f (%.1f)", g_target_yaw,
        atan2f(guest_float(e + 84), guest_float(e + 80)), atan2f(guest_float(c + 92), guest_float(c + 88)),
        angle(e + 348), reach(e + 348), angle(e + 372), reach(e + 372), angle(c + 300), reach(c + 300));
}

bool mouse_turning()
{
    return host_now_ns() - g_last_move_ns.load(std::memory_order_relaxed) < TURNING_NS;
}
