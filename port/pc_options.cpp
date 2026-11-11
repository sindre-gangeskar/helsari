// The GRAPHICS, GAMEPLAY and KEYBINDINGS sections of the game's own options menu. menu_patch.cpp adds their lists and rows to
// the page layout; the page's script is compiled and cannot be edited, so this runs Lua in the live
// script when the page opens (adds the section, shows the values) and takes the rows' clicks at the
// game's URL dispatcher. Settings persist in settings.ini.
#include "keybinds.h"

#include "runtime.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <map>
#include <vector>

extern "C"
{
    void f_004894e0_guest(PPU_CTX); // URL dispatcher: (this, const char *url)
    void f_00487fd0_guest(PPU_CTX); // calls a script function: (script, const String &call, ...)
    void f_003f1750(PPU_CTX);       // script wrapper: loads the script if needed (wrapper)
    void f_003fc7d8(PPU_CTX);       // lua_gettop(L)
    void f_003fca50(PPU_CTX);       // lua_settop(L, index)
    void f_003fd6f0(PPU_CTX);       // lua_getfield(L, index, name)
    void f_003fd220(PPU_CTX);       // lua_pushstring(L, s)
    void f_003fe238(PPU_CTX);       // lua_pcall(L, nargs, nresults, errfunc)
    void f_003fcdd0(PPU_CTX);       // lua_tolstring(L, index, len)
}
void gcm_pc_command(const std::string &cmd);                 // hle_gcm.cpp
void gcm_set_window(int width, int height, bool borderless); // hle_gcm.cpp
bool mouse_in_play();
bool mouse_menu_update();              // mouse.cpp
bool mouse_menu_point(int &x, int &y);
bool mouse_menu_clicked();
void mouse_menu_press(bool accept);
void mouse_menu_step(bool down);
bool mouse_menu_pressing();

namespace rsx
{
    void set_output_size(uint32_t width); // rsx/d3d.cpp
}

namespace
{

    constexpr int64_t LUA_GLOBALSINDEX = -10002;
    const char *const SETTINGS_FILE = "settings.ini";
    const char *const FPS[] = {"30", "60", "120", "UNLIMITED"};
    const struct
    {
        int w, h;
    } RES[] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    constexpr int FPS_N = 4, RES_N = 5;
    const float SWAY[] = {0.25f, 0.5f, 0.75f, 1.f, 1.25f, 1.5f, 1.75f, 2.f};
    constexpr int SWAY_N = 8;

    int g_fps = 1, g_res = 0;
    bool g_borderless = false;
    std::atomic<bool> g_vsync{false};
    // The game makes both effects in one image, and its depth of field needs the job that makes the motion
    // blur: motion blur without depth of field works, the other way round does not.
    const char *const SCREEN_FX[] = {"MOTION BLUR + DEPTH OF FIELD", "MOTION BLUR ONLY", "OFF"};
    constexpr int SCREEN_FX_N = 3;
    std::atomic<int> g_screen_fx{0};
    // Horizontal field of view in degrees, 74 to 110; 0 is the game's own, 72.7, shown as 73 on its slider.
    constexpr int FOV_MIN = 73, FOV_MAX = 110;
    std::atomic<int> g_fov{0};
    std::atomic<int> g_sway{3};
    std::atomic<bool> g_skip_intro{false}; // the movie before the start screen
    // Rendering API, chosen at start (the device is made once): 0 DX11, 1 DX12. A change shows
    // "restart" until the next start.
    const char *const RENDER_API[] = {"DX11", "DX12"};
    std::atomic<int> g_render_api{0};
    int g_render_api_running = -1;         // what this process runs, once the renderer asked
    std::atomic<uint32_t> g_L{0}; // Lua state of the page's script
    std::mutex g_lock;

    void save()
    {
        if (FILE *f = fopen(SETTINGS_FILE, "w"))
        {
            fprintf(f, "framerate_limit=%s\nresolution=%dx%d\nborderless=%d\nvsync=%d\nscreen_fx=%d\nfield_of_view=%d\nviewmodel_sway=%.2f\nskip_intro_movie=%d\nrender_api=%s\n", FPS[g_fps], RES[g_res].w, RES[g_res].h,
                    g_borderless, (int)g_vsync.load(), g_screen_fx.load(), g_fov.load(), SWAY[g_sway.load()], (int)g_skip_intro.load(),
                    RENDER_API[g_render_api.load()]);
            for (int i = 0; i < KEYBIND_COUNT; i++)
                fprintf(f, "key_%s=%d\n", keybind_id(i), keybind_key(i));
            fclose(f);
        }
    }

    // Read once, by whichever thread needs the settings first (the renderer may ask for the
    // rendering API before the window thread loads the rest).
    void load_file();
    void load()
    {
        static std::once_flag once;
        std::call_once(once, load_file);
    }
    void load_file()
    {
        FILE *f = fopen(SETTINGS_FILE, "r");
        if (!f)
            return;
        char line[128], s[64];
        int a, b;
        float v;
        while (fgets(line, sizeof line, f))
        {
            if (sscanf(line, "framerate_limit=%63s", s) == 1)
            {
                for (int i = 0; i < FPS_N; i++)
                    if (!strcmp(s, FPS[i]))
                        g_fps = i;
            }
            else if (sscanf(line, "resolution=%dx%d", &a, &b) == 2)
            {
                for (int i = 0; i < RES_N; i++)
                    if (RES[i].w == a && RES[i].h == b)
                        g_res = i;
            }
            else if (sscanf(line, "borderless=%d", &a) == 1)
            {
                g_borderless = a != 0;
            }
            else if (sscanf(line, "vsync=%d", &a) == 1)
            {
                g_vsync = a != 0;
            }
            else if (sscanf(line, "render_api=%63s", s) == 1)
            {
                g_render_api = !strcmp(s, "DX12") ? 1 : 0;
            }
            else if (sscanf(line, "skip_intro_movie=%d", &a) == 1)
            {
                g_skip_intro = a != 0;
            }
            else if (sscanf(line, "screen_fx=%d", &a) == 1)
            {
                g_screen_fx = a >= 0 && a < SCREEN_FX_N ? a : 0;
            }
            else if (sscanf(line, "field_of_view=%d", &a) == 1)
            {
                g_fov = a > FOV_MIN && a <= FOV_MAX ? a : 0;
            }
            else if (sscanf(line, "key_%63[^=]=%d", s, &a) == 2)
            {
                for (int i = 0; i < KEYBIND_COUNT; i++)
                    if (!strcmp(s, keybind_id(i)))
                        keybind_set(i, a);
            }
            else if (sscanf(line, "viewmodel_sway=%f", &v) == 1)
            {
                for (int i = 0; i < SWAY_N; i++)
                    if (SWAY[i] == v)
                        g_sway = i;
            }
        }
        fclose(f);
    }

    void apply_fps()
    {
        if (!getenv("HELSARI_VBLANK_US"))
            gcm_pc_command(std::string("fps=") + FPS[g_fps]);
    }
    void apply_window()
    {
        gcm_set_window(RES[g_res].w, RES[g_res].h, g_borderless);
        rsx::set_output_size((uint32_t)RES[g_res].w);
    }

    // Runs Lua source in state L, on the guest stack of the calling thread. Registers are restored.
    void lua_run(PPUContext *ctx, uint32_t L, const std::string &code)
    {
        PPUContext saved = *ctx;
        const uint64_t sp = (saved.r[1] - 1024 - code.size()) & ~15ull;
        st64(sp, saved.r[1]);
        const uint32_t name = (uint32_t)sp + 112, src = name + 32;
        memcpy(mem::ptr(name), "loadstring", 11);
        memcpy(mem::ptr(src), code.c_str(), code.size() + 1);
        ctx->r[1] = sp;
        auto report = [&](const char *what)
        {
            ctx->r[3] = L, ctx->r[4] = (uint64_t)-1ll, ctx->r[5] = 0;
            f_003fcdd0(ctx);
            const uint32_t msg = (uint32_t)ctx->r[3];
            LOG("menu", "Lua %s failed: %s", what, msg ? (const char *)mem::ptr(msg) : "(no message)");
        };
        // Lua stack slot of the value at index: 8 bytes, type tag at +4 (6 is a function, 0 is nil).
        auto type_at = [&](int back)
        { return ld32(ld32(L + 8) - 8 * back + 4); };
        ctx->r[3] = L;
        f_003fc7d8(ctx);
        const int64_t top = (int32_t)ctx->r[3];
        ctx->r[3] = L, ctx->r[4] = (uint64_t)LUA_GLOBALSINDEX, ctx->r[5] = name;
        f_003fd6f0(ctx);
        if (type_at(1) != 6)
        { // this state has no such function
            ctx->r[3] = L, ctx->r[4] = (uint64_t)top;
            f_003fca50(ctx);
            *ctx = saved;
            return;
        }
        ctx->r[3] = L, ctx->r[4] = src;
        f_003fd220(ctx);
        ctx->r[3] = L, ctx->r[4] = 1, ctx->r[5] = 1, ctx->r[6] = 0;
        f_003fe238(ctx); // loadstring(code) -> the function, or nil
        if ((int32_t)ctx->r[3])
        {
            report("loadstring call");
        }
        else if (type_at(1) != 6)
        {
            LOG("menu", "Lua did not compile: %s", code.c_str());
        }
        else
        {
            ctx->r[3] = L, ctx->r[4] = 0, ctx->r[5] = 0, ctx->r[6] = 0;
            f_003fe238(ctx);
            if ((int32_t)ctx->r[3])
                report("run");
        }
        ctx->r[3] = L, ctx->r[4] = (uint64_t)top;
        f_003fca50(ctx);
        *ctx = saved;
    }

    std::string set_text(const char *tag, const std::string &text)
    {
        return std::string("pc_set(\"") + tag + "\", \"" + text + "\")\n";
    }
    // Lua errors must not happen here: the game's error path crashes in this runtime (a type error dies
    // building its message, reading a bad state), so every tag is checked before use.
    const char *const LUA_SET =
        "local function pc_set(n, v)\n"
        "local t = gBrowser:GetTagByName(n)\n"
        "if t then t:SetValue(v) end\n"
        "end\n";
    std::string show_keys()
    {
        std::string out;
        for (int i = 0; i < KEYBIND_COUNT; i++)
            out += set_text(("tK" + std::to_string(i)).c_str(), keybind_row(i));
        return out;
    }
    std::string show_values()
    {
        char res[48], sway[48], fov[48];
        const int fov_degrees = g_fov.load() ? g_fov.load() : FOV_MIN;
        snprintf(fov, sizeof fov, "FIELD OF VIEW: %d", fov_degrees);
        snprintf(res, sizeof res, "RESOLUTION: %d X %d", RES[g_res].w, RES[g_res].h);
        snprintf(sway, sizeof sway, "VIEWMODEL SWAY MULTIPLIER: %.2fX", SWAY[g_sway.load()]);
        return LUA_SET +
               set_text("tF", std::string("FRAMERATE LIMIT: ") + FPS[g_fps]) + set_text("tR", res) +
               set_text("tM", g_borderless ? "WINDOW MODE: BORDERLESS" : "WINDOW MODE: WINDOWED") +
               set_text("tV", g_vsync ? "VSYNC: ON" : "VSYNC: OFF") +
               set_text("tB", std::string("SCREEN FX: ") + SCREEN_FX[g_screen_fx.load()]) + set_text("tO", fov) + set_text("tS", sway) +
               set_text("tA", std::string("RENDERING API: ") + RENDER_API[g_render_api.load()] +
                                  (g_render_api_running >= 0 && g_render_api.load() != g_render_api_running ? " (RESTART TO APPLY)" : "")) +
               set_text("tI", g_skip_intro ? "INTRO MOVIE: SKIP" : "INTRO MOVIE: PLAY") + show_keys() +
               "local fov_slider = gBrowser:GetTagByName(\"sO\")\nif fov_slider then fov_slider:SetCurrentValue(" + std::to_string(fov_degrees) + ") end\n";
    }

    // The mouse in menus. A menu row is a widget with this vtable; +4 is 3 while it is on screen and 2 in a
    // hidden list; +0x10 its id, +0x1c y, +0x20 x, +0x24 width (or -1), +0x28 height as floats; +0x30 to +0x3c
    // its list's screen rectangle. The script side has no way to ask where a tag is.
    struct MenuRow
    {
        uint32_t id, at;
        int x0, y0, x1, y1;
        int list_x0, list_y0, list_x1, list_y1, index; // its list's rectangle, its place in it
    };

    std::vector<MenuRow> menu_rows()
    {
        const auto as_float = [](uint32_t be) { const uint32_t u = __builtin_bswap32(be); float f; memcpy(&f, &u, 4); return f; };
        std::vector<MenuRow> rows;
        for (uint64_t ea = 0x30000000; ea < 0x60000000 && rows.size() < 200;)
        {
            MEMORY_BASIC_INFORMATION mb{};
            uint8_t *host = (uint8_t *)mem::ptr((uint32_t)ea);
            if (!VirtualQuery(host, &mb, sizeof mb))
                break;
            const uint64_t left = (uint8_t *)mb.BaseAddress + mb.RegionSize - host;
            if (mb.State == MEM_COMMIT && !(mb.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            {
                const uint32_t *w = (const uint32_t *)host, vt = __builtin_bswap32(0x00faa9e4u), shown = __builtin_bswap32(3u);
                for (uint64_t i = 0; i + 24 < left / 4 && rows.size() < 200; i++)
                    if (w[i] == vt && w[i + 1] == shown)
                    {
                        const int list_x0 = (int)__builtin_bswap32(w[i + 12]), list_y0 = (int)__builtin_bswap32(w[i + 13]), list_x1 = (int)__builtin_bswap32(w[i + 14]), list_y1 = (int)__builtin_bswap32(w[i + 15]);
                        const float y = as_float(w[i + 7]), x = as_float(w[i + 8]), wide = as_float(w[i + 9]), high = as_float(w[i + 10]);
                        if (list_x1 <= list_x0 || high <= 0.f)
                            continue;
                        MenuRow r{__builtin_bswap32(w[i + 4]), (uint32_t)ea + (uint32_t)i * 4, list_x0 + (int)x, list_y0 + (int)y, wide > 0.f ? list_x0 + (int)(x + wide) : list_x1, list_y0 + (int)(y + high),
                                  list_x0, list_y0, list_x1, list_y1, (int)(y / high + 0.5f)};
                        if (r.y1 > list_y0 && r.y0 < list_y1)
                            rows.push_back(r);
                    }
            }
            ea += left ? left : 0x1000;
        }
        return rows;
    }

    // Every update of a page: the row under the cursor gets the highlight, by pressing up or down as the game
    // expects, so its own navigation does the rest (sounds, descriptions, scrolling). +0x5c of a row has its top
    // byte set while it is its list's highlighted row. Only a list with a highlighted row has the focus: a click
    // on another goes back, which is how the game returns to the column it came from.
    // HELSARI_MENU_PROBE: on each click, what the page's script sees, and the flags of the visible rows.
    void menu_probe(PPUContext *ctx, uint32_t L, const std::vector<MenuRow> &rows)
    {
        std::string ids;
        for (const MenuRow &r : rows)
        {
            ids += std::to_string(r.id) + ",";
            LOG("menu", "probe row id %x at %08x index %d flags %08x list rect %d %d %d %d", r.id, r.at, r.index, ld32(r.at + 0x5c), r.list_x0, r.list_y0, r.list_x1, r.list_y1);
        }
        lua_run(ctx, L, "pc_probe = 'browser ' .. type(gBrowser)\n"
                        "if gBrowser then\n"
                        "local s = gBrowser:GetSelectedTag()\n"
                        "pc_probe = pc_probe .. ' selected ' .. type(s)\n"
                        "if s then pc_probe = pc_probe .. ' id ' .. tostring(s:GetId()) .. ' name ' .. tostring(s:GetName()) .. ' type ' .. tostring(s:GetTypeName()) end\n"
                        "for _, id in ipairs({" + ids + "}) do\n"
                        "local t = gBrowser:GetTagById(id)\n"
                        "pc_probe = pc_probe .. ' | ' .. id .. ' ' .. type(t)\n"
                        "if t then pc_probe = pc_probe .. ' ' .. tostring(t:GetName()) .. ' ' .. tostring(t:GetTypeName()) .. ' focus ' .. tostring(t:HasFocus()) .. ' selected ' .. tostring(t:IsSelected()) end\n"
                        "end\n"
                        "end\n");
        PPUContext saved = *ctx;
        const uint64_t sp = (saved.r[1] - 256) & ~15ull;
        st64(sp, saved.r[1]);
        const uint32_t name = (uint32_t)sp + 112;
        memcpy(mem::ptr(name), "pc_probe", 9);
        ctx->r[1] = sp;
        ctx->r[3] = L;
        f_003fc7d8(ctx);
        const int64_t top = (int32_t)ctx->r[3];
        ctx->r[3] = L, ctx->r[4] = (uint64_t)LUA_GLOBALSINDEX, ctx->r[5] = name;
        f_003fd6f0(ctx);
        ctx->r[3] = L, ctx->r[4] = (uint64_t)-1ll, ctx->r[5] = 0;
        f_003fcdd0(ctx);
        const uint32_t text = (uint32_t)ctx->r[3];
        std::string out = text ? (const char *)mem::ptr(text) : "(no text: the script stopped early)";
        for (size_t i = 0; i < out.size(); i += 1500)
            LOG("menu", "probe: %s", out.substr(i, 1500).c_str());
        ctx->r[3] = L, ctx->r[4] = (uint64_t)top;
        f_003fca50(ctx);
        *ctx = saved;
    }

    // The value of a Lua global as text ("" if it is not set). Lua errors crash this runtime, so the code that
    // sets the global must not be able to fail.
    std::string lua_global(PPUContext *ctx, uint32_t L, const char *global)
    {
        PPUContext saved = *ctx;
        const uint64_t sp = (saved.r[1] - 256) & ~15ull;
        st64(sp, saved.r[1]);
        const uint32_t name = (uint32_t)sp + 112;
        memcpy(mem::ptr(name), global, strlen(global) + 1);
        ctx->r[1] = sp;
        ctx->r[3] = L;
        f_003fc7d8(ctx);
        const int64_t top = (int32_t)ctx->r[3];
        ctx->r[3] = L, ctx->r[4] = (uint64_t)LUA_GLOBALSINDEX, ctx->r[5] = name;
        f_003fd6f0(ctx);
        ctx->r[3] = L, ctx->r[4] = (uint64_t)-1ll, ctx->r[5] = 0;
        f_003fcdd0(ctx);
        const uint32_t text = (uint32_t)ctx->r[3];
        std::string out = text ? (const char *)mem::ptr(text) : "";
        ctx->r[3] = L, ctx->r[4] = (uint64_t)top;
        f_003fca50(ctx);
        *ctx = saved;
        return out;
    }

    // Debugging: HELSARI_LUA_WATCH="name,name" logs those script globals whenever their value
    // changes, checked ten times a second while a page updates. Reads only, runs no Lua code.
    void lua_watch(PPUContext *ctx, uint32_t L)
    {
        static const std::vector<std::string> names = [] {
            std::vector<std::string> v;
            if (const char *p = getenv("HELSARI_LUA_WATCH"))
                for (std::string s = p; !s.empty();)
                {
                    const size_t c = s.find(',');
                    v.push_back(s.substr(0, c));
                    s = c == std::string::npos ? "" : s.substr(c + 1);
                }
            return v;
        }();
        static std::map<std::string, std::string> seen;
        static uint64_t last;
        const uint64_t now = host_now_ns();
        if (names.empty() || now - last < 100000000ull)
            return;
        last = now;
        for (const std::string &n : names)
        {
            std::string v = lua_global(ctx, L, n.c_str());
            if (seen.count(n) && seen[n] == v)
                continue;
            seen[n] = v;
            LOG("lua", "watch: %s = %s", n.c_str(), v.empty() ? "(nil or not text)" : v.c_str());
        }
    }

    // The Field of View slider: read while the page updates, ten times a second.
    void fov_slider(PPUContext *ctx, uint32_t L)
    {
        static uint64_t last;
        const uint64_t now = host_now_ns();
        if (now - last < 100000000ull)
            return;
        last = now;
        lua_run(ctx, L, "pc_fov = nil\nif gBrowser then local s = gBrowser:GetTagByName(\"sO\") if s and s:IsVisible() then pc_fov = s:GetCurrentValue() end end\n");
        const std::string text = lua_global(ctx, L, "pc_fov");
        if (text.empty())
            return;
        const int degrees = (int)(atof(text.c_str()) + 0.5);
        const int value = degrees > FOV_MIN && degrees <= FOV_MAX ? degrees : 0;
        if (value == g_fov.load())
            return;
        std::lock_guard<std::mutex> lk(g_lock);
        g_fov = value;
        save();
        char fov[48];
        snprintf(fov, sizeof fov, "FIELD OF VIEW: %d", value ? value : FOV_MIN);
        lua_run(ctx, L, LUA_SET + set_text("tO", fov));
    }

    void menu_mouse(PPUContext *ctx, uint32_t L)
    {
        static const bool off = getenv("HELSARI_NO_MENU_MOUSE") != nullptr;
        if (off || !mouse_menu_update())
            return;
        int x, y;
        const bool moved = mouse_menu_point(x, y), clicked = mouse_menu_clicked();
        static uint64_t last_scan, next_press;
        static std::vector<MenuRow> rows;
        static uint32_t target_at;      // the row the highlight is on its way to, 0 for none
        static int target_index, presses;
        static bool click_waiting;
        const uint64_t now = host_now_ns();
        if (moved || clicked)
        {
            if (clicked || now - last_scan > 100000000ull)
                rows = menu_rows(), last_scan = now;
            const MenuRow *row = nullptr;
            for (const MenuRow &r : rows)
                if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1)
                    row = &r;
            static const bool probe = getenv("HELSARI_MENU_PROBE") != nullptr;
            if (probe && clicked)
            {
                menu_probe(ctx, L, rows);
                return;
            }
            if (row)
            {
                target_at = row->at, target_index = row->index, presses = 0;
                click_waiting = click_waiting || clicked;
            }
            else
            {
                target_at = 0;
                click_waiting = false;
            }
        }
        if (!target_at || now < next_press || mouse_menu_pressing())
            return;
        if (ld32(target_at) != 0x00faa9e4u || ld32(target_at + 4) != 3u)
        {
            target_at = 0, click_waiting = false; // another page by now
            return;
        }
        const auto highlighted = [](const MenuRow &r) { return (ld32(r.at + 0x5c) >> 24) != 0; };
        const MenuRow *row = nullptr, *current = nullptr;
        for (const MenuRow &r : rows)
            if (r.at == target_at)
                row = &r;
        if (!row)
        {
            target_at = 0, click_waiting = false;
            return;
        }
        for (const MenuRow &r : rows)
            if (r.list_x0 == row->list_x0 && r.list_y0 == row->list_y0 && r.list_x1 == row->list_x1 && r.list_y1 == row->list_y1 && highlighted(r))
                current = &r;
        if (!current)
        {
            if (click_waiting)
                mouse_menu_press(false), next_press = now + 200000000ull;
            target_at = 0, click_waiting = false;
            return;
        }
        if (current->index == target_index)
        {
            if (click_waiting)
                mouse_menu_press(true), next_press = now + 200000000ull;
            target_at = 0, click_waiting = false;
            return;
        }
        if (presses++ > 40)
        {
            target_at = 0, click_waiting = false;
            return;
        }
        mouse_menu_step(target_index > current->index);
        next_press = now + 90000000ull;
    }

    bool menu_on() { return !getenv("HELSARI_MENU_PATCH") || atoi(getenv("HELSARI_MENU_PATCH")); }

} // namespace

namespace rsx
{
    bool pc_vsync() { return g_vsync.load(std::memory_order_relaxed); }
    int pc_field_of_view() { return g_fov.load(std::memory_order_relaxed); }
    bool pc_motion_blur() { return g_screen_fx.load(std::memory_order_relaxed) != 2; }
    bool pc_depth_of_field() { return g_screen_fx.load(std::memory_order_relaxed) == 0; }
}

// The rendering API this process runs (asked once, when the renderer starts).
int pc_render_api()
{
    load();
    if (g_render_api_running < 0)
        g_render_api_running = g_render_api.load();
    return g_render_api_running;
}
bool pc_skip_intro_movie() { return g_skip_intro.load(std::memory_order_relaxed); }
float pc_viewmodel_sway() { return SWAY[g_sway.load(std::memory_order_relaxed)]; }

// Window thread, once the window exists.
void pc_options_start()
{
    load();
    apply_fps();
    apply_window();
    LOG("menu", "settings: framerate limit %s, %dx%d, %s, vsync %d", FPS[g_fps], RES[g_res].w, RES[g_res].h,
        g_borderless ? "borderless" : "windowed", (int)g_vsync.load());
}

extern "C" void f_00487fd0(PPU_CTX)
{
    const uint32_t script = (uint32_t)ctx->r[3], call_ref = (uint32_t)ctx->r[4];
    if (!menu_on() || !call_ref)
    {
        f_00487fd0_guest(ctx);
        return;
    }
    std::string call;
    if (uint32_t s = ld32(call_ref))
        call = (const char *)mem::ptr(s + 16);
    // Debugging: HELSARI_TRACE_SCRIPT_CALLS=1 logs every script call the menu makes; page
    // updates (every frame) are only counted, once a second.
    static const bool trace_calls = getenv("HELSARI_TRACE_SCRIPT_CALLS") != nullptr;
    if (trace_calls)
    {
        static uint64_t last, updates;
        if (call == "OnUpdatePage")
            updates++;
        else
            LOG("script", "call %s (script 0x%x)", call.c_str(), script);
        const uint64_t now = host_now_ns();
        if (now - last >= 1000000000ull)
            LOG("script", "%llu page updates in the last second", (unsigned long long)updates), updates = 0, last = now;
    }
    // Before the call: a script can open another page from OnEnterPage, which destroys its state.
    // The options table exists by now, it is built at the script's top level.
    // Scripts load lazily: the wrapper at M checks its load state (+12) before each use and loads
    // the script if that is not 3 (f_003f1858), and only then is its Lua state (+20) usable.
    const uint32_t M = ld32(script + 364) + 8;
    if (call == "OnEnterPage" && ld32(M + 12) != 3)
    {
        PPUContext saved = *ctx;
        ctx->r[3] = M;
        f_003f1750(ctx);
        *ctx = saved;
    }
    const uint32_t state = ld32(M + 12), L = state == 3 ? ld32(M + 20) : 0, G = L ? ld32(L + 16) : 0;
    if (G)
        g_L = L; // a script of the page in use: any of them can reach the page's tags
    if (G && call == "OnUpdatePage")
        menu_mouse(ctx, L), fov_slider(ctx, L), lua_watch(ctx, L);
    if (G && call == "OnEnterPage")
    {
        std::lock_guard<std::mutex> lk(g_lock);
        // Only pages that have our list: the main menu has an options table of its own but no rows yet.
        lua_run(ctx, L, "if gOptionsTable and gBrowser and gBrowser:GetTagByName(\"lG\") and gBrowser:GetTagByName(\"lP\") and gBrowser:GetTagByName(\"lK\") then\n"
                        "local l = gOptionsTable.links\n"
                        "local n = getn(l)\n"
                        "if l[n].sub_menu_list ~= \"lK\" then\n"
                        "l[n+1] = { title = \"GRAPHICS\", link = \"static://script.ingameoptionsscript.ShowOptionsSubMenu(\"..(n+1)..\")\", "
                        "sub_menu_list = \"lG\", description = \"Change the graphics settings\", links = {} }\n"
                        "l[n+2] = { title = \"GAMEPLAY\", link = \"static://script.ingameoptionsscript.ShowOptionsSubMenu(\"..(n+2)..\")\", "
                        "sub_menu_list = \"lP\", description = \"Change the gameplay settings\", links = {} }\n"
                        "l[n+3] = { title = \"KEYBINDINGS\", link = \"static://script.ingameoptionsscript.ShowOptionsSubMenu(\"..(n+3)..\")\", "
                        "sub_menu_list = \"lK\", description = \"Change the keyboard and mouse controls\", links = {} }\n"
                        // Showing the options panel shows every list in it, then the game hides its own by
                        // name. Lua finds functions by name at each call, so this covers the compiled callers.
                        "local hide = HideAllSettingOptions\n"
                        "function HideAllSettingOptions()\n"
                        "hide()\n"
                        "gBrowser:GetTagByName(\"lG\"):SetVisible(false)\n"
                        "gBrowser:GetTagByName(\"lP\"):SetVisible(false)\n"
                        "gBrowser:GetTagByName(\"lK\"):SetVisible(false)\n"
                        "end\n"
                        "end\n"
                        "gBrowser:GetTagByName(\"lG\"):SetVisible(false)\n"
                        "gBrowser:GetTagByName(\"lP\"):SetVisible(false)\n"
                        "gBrowser:GetTagByName(\"lK\"):SetVisible(false)\n" +
                            show_values() + "end\n");
    }
    f_00487fd0_guest(ctx);
}

extern "C" void f_004894e0(PPU_CTX)
{
    static const std::string prefix = "static://hs/";
    std::string url;
    if (menu_on() && (uint32_t)ctx->r[4])
        url = (const char *)mem::ptr((uint32_t)ctx->r[4]);
    if (url.compare(0, prefix.size(), prefix) != 0)
    {
        f_004894e0_guest(ctx);
        return;
    }
    const std::string row = url.substr(prefix.size());
    LOG("menu", "row selected: %s", row.c_str());
    std::lock_guard<std::mutex> lk(g_lock);
    // A key was just taken: this press only came to bring us onto the script's thread.
    if (keybind_take_refresh())
    {
    }
    else if (row[0] == 'k')
        keybind_begin_capture(atoi(row.c_str() + 1));
    else if (row == "fps")
        g_fps = (g_fps + 1) % FPS_N, apply_fps();
    else if (row == "res")
        g_res = (g_res + 1) % RES_N, apply_window();
    else if (row == "mode")
        g_borderless = !g_borderless, apply_window();
    else if (row == "vsync")
        g_vsync = !g_vsync;
    else if (row == "fx")
        g_screen_fx = (g_screen_fx.load() + 1) % SCREEN_FX_N;
    else if (row == "fov")
        return; // its slider sets it
    else if (row == "sway")
        g_sway = (g_sway.load() + 1) % SWAY_N;
    else if (row == "intro")
        g_skip_intro = !g_skip_intro;
    else if (row == "api")
        g_render_api = !g_render_api.load();
    save();
    if (const uint32_t L = g_L.load())
        lua_run(ctx, L, show_values());
}
