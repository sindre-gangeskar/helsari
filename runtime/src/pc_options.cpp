// The GRAPHICS and GAMEPLAY sections of the game's own options menu. menu_patch.cpp adds their lists and rows to
// the page layout; the page's script is compiled and cannot be edited, so this runs Lua in the live
// script when the page opens (adds the section, shows the values) and takes the rows' clicks at the
// game's URL dispatcher. Settings persist in settings.ini.
#include "runtime.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

extern "C"
{
    void f_0047ccc0_guest(PPU_CTX); // URL dispatcher: (this, const char *url)
    void f_0047b900_guest(PPU_CTX); // calls a script function: (script, const String &call, ...)
    void f_003e5300(PPU_CTX);       // script wrapper: loads the script if needed (wrapper)
    void f_003f0388(PPU_CTX);       // lua_gettop(L)
    void f_003f0600(PPU_CTX);       // lua_settop(L, index)
    void f_003f12a0(PPU_CTX);       // lua_getfield(L, index, name)
    void f_003f0dd0(PPU_CTX);       // lua_pushstring(L, s)
    void f_003f1de8(PPU_CTX);       // lua_pcall(L, nargs, nresults, errfunc)
    void f_003f0980(PPU_CTX);       // lua_tolstring(L, index, len)
}
void gcm_pc_command(const std::string &cmd);                 // hle_gcm.cpp
void gcm_set_window(int width, int height, bool borderless); // hle_gcm.cpp

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
    std::atomic<int> g_sway{3};
    std::atomic<uint32_t> g_L{0}; // Lua state of the page's script
    std::mutex g_lock;

    void save()
    {
        if (FILE *f = fopen(SETTINGS_FILE, "w"))
        {
            fprintf(f, "framerate_limit=%s\nresolution=%dx%d\nborderless=%d\nvsync=%d\nviewmodel_sway=%.2f\n", FPS[g_fps], RES[g_res].w, RES[g_res].h,
                    g_borderless, (int)g_vsync.load(), SWAY[g_sway.load()]);
            fclose(f);
        }
    }

    void load()
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
        if (!getenv("KZ2_VBLANK_US"))
            gcm_pc_command(std::string("fps=") + FPS[g_fps]);
    }
    void apply_window() { gcm_set_window(RES[g_res].w, RES[g_res].h, g_borderless); }

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
            f_003f0980(ctx);
            const uint32_t msg = (uint32_t)ctx->r[3];
            LOG("menu", "Lua %s failed: %s", what, msg ? (const char *)mem::ptr(msg) : "(no message)");
        };
        // Lua stack slot of the value at index: 8 bytes, type tag at +4 (6 is a function, 0 is nil).
        auto type_at = [&](int back)
        { return ld32(ld32(L + 8) - 8 * back + 4); };
        ctx->r[3] = L;
        f_003f0388(ctx);
        const int64_t top = (int32_t)ctx->r[3];
        ctx->r[3] = L, ctx->r[4] = (uint64_t)LUA_GLOBALSINDEX, ctx->r[5] = name;
        f_003f12a0(ctx);
        if (type_at(1) != 6)
        { // this state has no such function
            ctx->r[3] = L, ctx->r[4] = (uint64_t)top;
            f_003f0600(ctx);
            *ctx = saved;
            return;
        }
        ctx->r[3] = L, ctx->r[4] = src;
        f_003f0dd0(ctx);
        ctx->r[3] = L, ctx->r[4] = 1, ctx->r[5] = 1, ctx->r[6] = 0;
        f_003f1de8(ctx); // loadstring(code) -> the function, or nil
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
            f_003f1de8(ctx);
            if ((int32_t)ctx->r[3])
                report("run");
        }
        ctx->r[3] = L, ctx->r[4] = (uint64_t)top;
        f_003f0600(ctx);
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
    std::string show_values()
    {
        char res[48], sway[48];
        snprintf(res, sizeof res, "RESOLUTION: %d X %d", RES[g_res].w, RES[g_res].h);
        snprintf(sway, sizeof sway, "VIEWMODEL SWAY MULTIPLIER: %.2fX", SWAY[g_sway.load()]);
        return LUA_SET +
               set_text("tF", std::string("FRAMERATE LIMIT: ") + FPS[g_fps]) + set_text("tR", res) +
               set_text("tM", g_borderless ? "WINDOW MODE: BORDERLESS" : "WINDOW MODE: WINDOWED") +
               set_text("tV", g_vsync ? "VSYNC: ON" : "VSYNC: OFF") + set_text("tS", sway);
    }

    bool menu_on() { return !getenv("KZ2_MENU_PATCH") || atoi(getenv("KZ2_MENU_PATCH")); }

} // namespace

namespace rsx
{
    bool pc_vsync() { return g_vsync.load(std::memory_order_relaxed); }
}

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

extern "C" void f_0047b900(PPU_CTX)
{
    const uint32_t script = (uint32_t)ctx->r[3], call_ref = (uint32_t)ctx->r[4];
    if (!menu_on() || !call_ref)
    {
        f_0047b900_guest(ctx);
        return;
    }
    std::string call;
    if (uint32_t s = ld32(call_ref))
        call = (const char *)mem::ptr(s + 16);
    // Before the call: a script can open another page from OnEnterPage, which destroys its state.
    // The options table exists by now, it is built at the script's top level.
    // Scripts load lazily: the wrapper at M checks its load state (+12) before each use and loads
    // the script if that is not 3 (f_003e5408), and only then is its Lua state (+20) usable.
    const uint32_t M = ld32(script + 364) + 8;
    if (call == "OnEnterPage" && ld32(M + 12) != 3)
    {
        PPUContext saved = *ctx;
        ctx->r[3] = M;
        f_003e5300(ctx);
        *ctx = saved;
    }
    const uint32_t state = ld32(M + 12), L = state == 3 ? ld32(M + 20) : 0, G = L ? ld32(L + 16) : 0;
    if (G)
        g_L = L; // a script of the page in use: any of them can reach the page's tags
    if (G && call == "OnEnterPage")
    {
        std::lock_guard<std::mutex> lk(g_lock);
        // Only pages that have our list: the main menu has an options table of its own but no rows yet.
        lua_run(ctx, L, "if gOptionsTable and gBrowser and gBrowser:GetTagByName(\"lG\") and gBrowser:GetTagByName(\"lP\") then\n"
                        "local l = gOptionsTable.links\n"
                        "local n = getn(l)\n"
                        "if l[n].sub_menu_list ~= \"lP\" then\n"
                        "l[n+1] = { title = \"GRAPHICS\", link = \"static://script.ingameoptionsscript.ShowOptionsSubMenu(\"..(n+1)..\")\", "
                        "sub_menu_list = \"lG\", description = \"Change the graphics settings\", links = {} }\n"
                        "l[n+2] = { title = \"GAMEPLAY\", link = \"static://script.ingameoptionsscript.ShowOptionsSubMenu(\"..(n+2)..\")\", "
                        "sub_menu_list = \"lP\", description = \"Change the gameplay settings\", links = {} }\n"
                        // Showing the options panel shows every list in it, then the game hides its own by
                        // name. Lua finds functions by name at each call, so this covers the compiled callers.
                        "local hide = HideAllSettingOptions\n"
                        "function HideAllSettingOptions()\n"
                        "hide()\n"
                        "gBrowser:GetTagByName(\"lG\"):SetVisible(false)\n"
                        "gBrowser:GetTagByName(\"lP\"):SetVisible(false)\n"
                        "end\n"
                        "end\n"
                        "gBrowser:GetTagByName(\"lG\"):SetVisible(false)\n"
                        "gBrowser:GetTagByName(\"lP\"):SetVisible(false)\n" +
                            show_values() + "end\n");
    }
    f_0047b900_guest(ctx);
}

extern "C" void f_0047ccc0(PPU_CTX)
{
    static const std::string prefix = "static://hs/";
    std::string url;
    if (menu_on() && (uint32_t)ctx->r[4])
        url = (const char *)mem::ptr((uint32_t)ctx->r[4]);
    if (url.compare(0, prefix.size(), prefix) != 0)
    {
        f_0047ccc0_guest(ctx);
        return;
    }
    const std::string row = url.substr(prefix.size());
    LOG("menu", "row selected: %s", row.c_str());
    std::lock_guard<std::mutex> lk(g_lock);
    if (row == "fps")
        g_fps = (g_fps + 1) % FPS_N, apply_fps();
    else if (row == "res")
        g_res = (g_res + 1) % RES_N, apply_window();
    else if (row == "mode")
        g_borderless = !g_borderless, apply_window();
    else if (row == "vsync")
        g_vsync = !g_vsync;
    else if (row == "sway")
        g_sway = (g_sway.load() + 1) % SWAY_N;
    save();
    if (const uint32_t L = g_L.load())
        lua_run(ctx, L, show_values());
}
