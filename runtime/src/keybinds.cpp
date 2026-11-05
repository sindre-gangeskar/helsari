// Keyboard and mouse bindings: what each key does in play, the "press a key" mode of the KEYBINDINGS
// section in the options menu (pc_options.cpp), and the prompts. On PC input the game's <buttonimage>
// tag shows the bound key as text instead of its PS3 icon; after controller input, the icon again.
#include "keybinds.h"

#include "idle_backoff.h"
#include "runtime.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

extern "C"
{
    void f_002d1888_guest(PPU_CTX); // the <buttonimage> tag: (text, args, flag)
    void f_00063c08(PPU_CTX);       // whether this region swaps the ok and cancel buttons
    void f_0050ac20(PPU_CTX);       // finds a tag argument: (text, args, "name=a|b|c", int *index, 0)
    void f_0050a2e8(PPU_CTX);       // adds one character to the text: (text, code)
}
bool mouse_in_play();       // mouse.cpp
bool mouse_look_active();   // mouse.cpp
void mouse_note_pc_input(); // mouse.cpp

namespace
{
    enum Kind
    {
        BUTTON,
        LEFT_TRIGGER,
        RIGHT_TRIGGER,
        STICK_X_MINUS,
        STICK_X_PLUS,
        STICK_Y_MINUS,
        STICK_Y_PLUS
    };
    struct Action
    {
        const char *id, *label;
        Kind kind;
        WORD button;
        int default_key;
    };
    // The game's Standard controller scheme, as a keyboard and mouse.
    const Action ACTIONS[KEYBIND_COUNT] = {
        {"forward", "MOVE FORWARD", STICK_Y_PLUS, 0, 'W'},
        {"back", "MOVE BACK", STICK_Y_MINUS, 0, 'S'},
        {"left", "MOVE LEFT", STICK_X_MINUS, 0, 'A'},
        {"right", "MOVE RIGHT", STICK_X_PLUS, 0, 'D'},
        {"fire", "FIRE", BUTTON, XINPUT_GAMEPAD_RIGHT_SHOULDER, VK_LBUTTON},
        {"aim", "AIM", BUTTON, XINPUT_GAMEPAD_RIGHT_THUMB, VK_RBUTTON},
        {"melee", "MELEE", BUTTON, XINPUT_GAMEPAD_LEFT_SHOULDER, 'V'},
        {"crouch", "CROUCH / COVER", LEFT_TRIGGER, 0, 'C'},
        {"grenade", "GRENADE", RIGHT_TRIGGER, 0, 'G'},
        {"sprint", "SPRINT", BUTTON, XINPUT_GAMEPAD_LEFT_THUMB, VK_SHIFT},
        {"jump", "JUMP", BUTTON, XINPUT_GAMEPAD_A, VK_SPACE},
        {"use", "INTERACT / PICK UP", BUTTON, XINPUT_GAMEPAD_B, 'E'}, // and the reload button during a pick up prompt: see keybind_apply
        {"reload", "RELOAD", BUTTON, XINPUT_GAMEPAD_X, 'R'},
        {"weapon", "NEXT WEAPON", BUTTON, XINPUT_GAMEPAD_Y, 'Q'},
    };
    enum
    {
        A_FORWARD,
        A_BACK,
        A_LEFT,
        A_RIGHT,
        A_FIRE,
        A_AIM,
        A_MELEE,
        A_CROUCH,
        A_GRENADE,
        A_SPRINT,
        A_JUMP,
        A_USE,
        A_RELOAD,
        A_WEAPON
    };

    std::atomic<int> g_keys[KEYBIND_COUNT];
    const bool g_keys_ready = []
    {
        for (int i = 0; i < KEYBIND_COUNT; i++)
            g_keys[i] = ACTIONS[i].default_key;
        return true;
    }();
    std::atomic<int> g_capture{-1};  // the action waiting for a key
    std::atomic<int> g_held_key{0};  // the key that ended it, until let go
    std::atomic<bool> g_refresh{false}; // the menu has to show the new key
    std::atomic<int> g_generation{0};
    std::atomic<uint64_t> g_square_prompt_ns{0}; // when the HUD last drew its square button

    bool down(int vk) { return GetAsyncKeyState(vk) & 0x8000; }

    std::string key_name(int vk)
    {
        switch (vk)
        {
        case VK_LBUTTON: return "MOUSE 0";
        case VK_RBUTTON: return "MOUSE 1";
        case VK_MBUTTON: return "MOUSE 2";
        case VK_XBUTTON1: return "MOUSE 3";
        case VK_XBUTTON2: return "MOUSE 4";
        case VK_SHIFT: return "SHIFT";
        case VK_CONTROL: return "CTRL";
        case VK_MENU: return "ALT";
        case VK_SPACE: return "SPACE";
        case VK_TAB: return "TAB";
        case VK_RETURN: return "ENTER";
        case VK_BACK: return "BACKSPACE";
        case VK_ESCAPE: return "ESC";
        case VK_CAPITAL: return "CAPS LOCK";
        case VK_UP: return "UP";
        case VK_DOWN: return "DOWN";
        case VK_LEFT: return "LEFT";
        case VK_RIGHT: return "RIGHT";
        }
        char name[32] = {};
        LONG scan = (LONG)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC) << 16;
        if (!GetKeyNameTextA(scan, name, sizeof name))
            snprintf(name, sizeof name, "KEY %d", vk);
        std::string s = name;
        for (char &c : s)
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 32);
        return s;
    }

    std::string bound(int action) { return key_name(g_keys[action].load(std::memory_order_relaxed)); }

    // What to show for a <buttonimage name=...>. Empty: it is not a button, keep the game's image.
    std::string prompt(const char *name, bool play)
    {
        const auto is = [&](const char *n) { return !strcmp(name, n); };
        if (is("ok"))
            return play ? bound(A_JUMP) : "ENTER";
        if (is("cancel"))
            return play ? bound(A_USE) : "ESC";
        if (is("square")) // a prompt with this button is about picking something up
            return play ? bound(A_USE) : bound(A_RELOAD);
        if (is("triangle"))
            return bound(A_WEAPON);
        if (is("L1"))
            return bound(A_MELEE);
        if (is("L2"))
            return bound(A_CROUCH);
        if (is("L3"))
            return bound(A_SPRINT);
        if (is("R1"))
            return bound(A_FIRE);
        if (is("R2"))
            return bound(A_GRENADE);
        if (is("R3"))
            return bound(A_AIM);
        if (is("start"))
            return "ESC";
        if (is("select"))
            return "BACKSPACE";
        if (is("left"))
            return "LEFT";
        if (is("right"))
            return "RIGHT";
        if (is("up"))
            return "UP";
        if (is("down"))
            return "DOWN";
        if (is("updown"))
            return "UP / DOWN";
        if (is("leftright"))
            return "LEFT / RIGHT";
        if (is("leftrightupdown"))
            return "ARROW KEYS";
        if (is("LAnalog"))
            return bound(A_FORWARD) + bound(A_LEFT) + bound(A_BACK) + bound(A_RIGHT);
        if (is("RAnalog"))
            return "MOUSE";
        return "";
    }
} // namespace

const char *keybind_id(int action) { return ACTIONS[action].id; }

int keybind_key(int action) { return g_keys[action].load(std::memory_order_relaxed); }

void keybind_set(int action, int vk)
{
    if (vk > 0 && vk < 256)
    {
        g_keys[action] = vk;
        g_generation++;
    }
}

std::string keybind_row(int action)
{
    const std::string key = g_capture.load() == action ? "PRESS A KEY (ESC CANCELS)" : bound(action);
    return std::string(ACTIONS[action].label) + ": " + key;
}

void keybind_begin_capture(int action)
{
    g_capture = action;
    LOG("keybind", "waiting for a key: %s", ACTIONS[action].id);
}

bool keybind_take_refresh() { return g_refresh.exchange(false); }

// Window thread.
void keybind_message(UINT m, WPARAM w, LPARAM l)
{
    const int action = g_capture.load();
    if (action < 0)
        return;
    int vk = 0;
    switch (m)
    {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (!(l & (1 << 30))) // not a repeat of the key that opened this
            vk = (int)w;
        break;
    case WM_LBUTTONDOWN: vk = VK_LBUTTON; break;
    case WM_RBUTTONDOWN: vk = VK_RBUTTON; break;
    case WM_MBUTTONDOWN: vk = VK_MBUTTON; break;
    case WM_XBUTTONDOWN: vk = HIWORD(w) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2; break;
    }
    if (!vk)
        return;
    if (vk != VK_ESCAPE)
    {
        // A key does one thing: whatever had it takes the key this action gives up.
        const int old = g_keys[action].load();
        for (int i = 0; i < KEYBIND_COUNT; i++)
            if (i != action && g_keys[i].load() == vk)
                g_keys[i] = old;
        g_keys[action] = vk;
        g_generation++;
        LOG("keybind", "%s = %s", ACTIONS[action].id, key_name(vk).c_str());
    }
    g_held_key = vk;
    g_capture = -1;
    g_refresh = true;
}

// Pad read: the keyboard and mouse as a pad, while the window has focus.
void keybind_apply(XINPUT_GAMEPAD &g)
{
    // Waiting for a key, and until that key is let go, the game sees nothing from the keyboard.
    if (g_capture.load() >= 0)
        return;
    if (const int held = g_held_key.load())
    {
        if (down(held))
            return;
        g_held_key = 0;
    }
    // Pressing a row reaches the game's script thread, which can then show the new key.
    if (g_refresh.load())
    {
        g.wButtons |= XINPUT_GAMEPAD_A;
        return;
    }
    bool any = false;
    const auto press = [&](int vk, WORD bit)
    {
        if (down(vk))
            g.wButtons |= bit, any = true;
    };
    press(VK_UP, XINPUT_GAMEPAD_DPAD_UP);
    press(VK_DOWN, XINPUT_GAMEPAD_DPAD_DOWN);
    press(VK_LEFT, XINPUT_GAMEPAD_DPAD_LEFT);
    press(VK_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT);
    press(VK_BACK, XINPUT_GAMEPAD_BACK);
    if (mouse_in_play())
    {
        press(VK_ESCAPE, XINPUT_GAMEPAD_START);
    }
    else
    {
        press(VK_RETURN, XINPUT_GAMEPAD_A);
        press(VK_ESCAPE, XINPUT_GAMEPAD_B);
    }
    for (int i = 0; i < KEYBIND_COUNT; i++)
    {
        if (!down(g_keys[i].load(std::memory_order_relaxed)))
            continue;
        any = true;
        switch (ACTIONS[i].kind)
        {
        case BUTTON: g.wButtons |= ACTIONS[i].button; break;
        case LEFT_TRIGGER: g.bLeftTrigger = 255; break;
        case RIGHT_TRIGGER: g.bRightTrigger = 255; break;
        case STICK_X_MINUS: g.sThumbLX = -32768; break;
        case STICK_X_PLUS: g.sThumbLX = 32767; break;
        case STICK_Y_MINUS: g.sThumbLY = -32768; break;
        case STICK_Y_PLUS: g.sThumbLY = 32767; break;
        }
    }
    // The pad picks up by holding the reload button, and a tap of it reloads. So the interact key only
    // reaches that button when held with the prompt on screen, and then stays on it until the prompt
    // goes (or a limit), since letting go early would be a tap.
    {
        constexpr uint64_t HOLD_FIRST_NS = 200000000ull, PROMPT_RECENT_NS = 200000000ull, MOST_NS = 2000000000ull;
        static uint64_t pressed_at, square_since;
        const uint64_t now = host_now_ns();
        const bool prompt_up = now - g_square_prompt_ns.load(std::memory_order_relaxed) < PROMPT_RECENT_NS;
        const bool held = down(g_keys[A_USE].load(std::memory_order_relaxed));
        if (!held)
            pressed_at = 0;
        else if (!pressed_at)
            pressed_at = now;
        if (square_since && (!prompt_up || now - square_since > MOST_NS))
            square_since = 0;
        if (!square_since && held && prompt_up && now - pressed_at > HOLD_FIRST_NS)
            square_since = now;
        if (square_since)
            g.wButtons |= XINPUT_GAMEPAD_X;
    }
    // Looking around without a mouse, for when the mouse look is switched off.
    if (down('J')) g.sThumbRX = -32768;
    if (down('L')) g.sThumbRX = 32767;
    if (down('I')) g.sThumbRY = 32767;
    if (down('K')) g.sThumbRY = -32768;
    if (any)
        mouse_note_pc_input();
}

int keybind_generation() { return g_generation.load(std::memory_order_relaxed); }

void keybind_note_square_prompt() { g_square_prompt_ns = host_now_ns(); }

// The button cells of the game's HUD icon atlas (1024x512), read off the atlas, each repainted as
// the bound key in white. Everything else on the sheet stays as it is.
void keybind_paint_hud(unsigned char *rgba, int width, int height)
{
    static const struct
    {
        int x, y, w, h;
        const char *name;
    } CELLS[] = {
        {0, 248, 38, 38, "square"}, {39, 248, 38, 38, "ok"}, {78, 248, 38, 38, "triangle"}, {117, 248, 38, 38, "cancel"}, {566, 172, 44, 44, "cancel"},
        {516, 211, 46, 24, "L1"}, {516, 239, 46, 24, "L1"}, {516, 269, 46, 24, "R1"}, {516, 297, 46, 24, "R1"},
        {516, 324, 56, 28, "R1"}, {516, 352, 46, 28, "L2"}, {516, 381, 46, 28, "R2"},
        {650, 278, 32, 26, "L3"}, {694, 278, 32, 26, "R3"},
        {950, 261, 62, 25, "L2"}, {950, 288, 62, 25, "R2"}, {950, 315, 62, 25, "R2"}, {950, 342, 62, 25, "L2"},
        {950, 369, 62, 25, "L1"}, {950, 396, 62, 25, "R1"}, {950, 423, 62, 25, "R1"}, {950, 450, 62, 25, "L1"},
    };
    HDC dc = CreateCompatibleDC(nullptr);
    for (const auto &cell : CELLS)
    {
        if (cell.x + cell.w > width || cell.y + cell.h > height)
            continue;
        const std::string label = prompt(cell.name, true); // the HUD is always in play
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = cell.w, bi.bmiHeader.biHeight = -cell.h;
        bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp)
            continue;
        HGDIOBJ old_bmp = SelectObject(dc, bmp);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        // The largest size that fits.
        HFONT font = nullptr;
        SIZE size{};
        for (int h = cell.h - 6; h >= 7; h--)
        {
            if (font)
                DeleteObject(font);
            font = CreateFontA(-h, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Arial");
            SelectObject(dc, font);
            GetTextExtentPoint32A(dc, label.c_str(), (int)label.size(), &size);
            if (size.cx <= cell.w - 6)
                break;
        }
        TextOutA(dc, (cell.w - size.cx) / 2, (cell.h - size.cy) / 2, label.c_str(), (int)label.size());
        GdiFlush();
        // Just the label, in white: its coverage is the alpha.
        const unsigned char *text = (const unsigned char *)bits;
        for (int y = 0; y < cell.h; y++)
            for (int x = 0; x < cell.w; x++)
            {
                unsigned char *out = rgba + ((cell.y + y) * width + cell.x + x) * 4;
                out[0] = out[1] = out[2] = 255;
                out[3] = text[(y * cell.w + x) * 4 + 1];
            }
        SelectObject(dc, old_bmp);
        if (font)
            DeleteObject(font);
        DeleteObject(bmp);
    }
    DeleteDC(dc);
}

extern "C" void f_002d1888(PPU_CTX)
{
    if (!mouse_look_active())
    {
        f_002d1888_guest(ctx);
        return;
    }
    static const char *const NAMES[] = {"", "triangle", "square", "", "left", "right", "up", "down", "L1", "L2", "L3", "R1", "R2", "R3", "updown",
                                        "cursor", "arrow", "HR", "asterix", "leftright", "trademark", "LAnalog", "RAnalog", "select", "start",
                                        "familylogo", "ps3logo", "scediamondlogo", "saveprofile", "leftrightupdown"};
    constexpr uint32_t CANCEL_FIRST = 0xbe6a8c, OK_FIRST = 0xbe6b64;
    PPUContext saved = *ctx;
    const uint32_t text = (uint32_t)ctx->r[3], args = (uint32_t)ctx->r[4];
    const uint64_t sp = (saved.r[1] - 512) & ~15ull;
    st64(sp, saved.r[1]);
    ctx->r[1] = sp;
    const uint32_t index_ea = (uint32_t)sp + 140;
    st32(index_ea, 0xffffffffu);
    ctx->r[3] = text;
    f_00063c08(ctx);
    const bool cancel_first = (int32_t)ctx->r[3] != 0;
    ctx->r[3] = text, ctx->r[4] = args, ctx->r[5] = cancel_first ? CANCEL_FIRST : OK_FIRST, ctx->r[6] = index_ea, ctx->r[7] = 0;
    f_0050ac20(ctx);
    const int index = (int32_t)ld32(index_ea);
    std::string label;
    if (index == 0)
        label = prompt(cancel_first ? "cancel" : "ok", mouse_in_play());
    else if (index == 3)
        label = prompt(cancel_first ? "ok" : "cancel", mouse_in_play());
    else if (index > 0 && index < (int)(sizeof NAMES / sizeof NAMES[0]))
        label = prompt(NAMES[index], mouse_in_play());
    if (label.empty())
    {
        *ctx = saved;
        f_002d1888_guest(ctx);
        return;
    }
    for (const char c : label)
    {
        ctx->r[3] = text, ctx->r[4] = (uint8_t)c;
        f_0050a2e8(ctx);
    }
    *ctx = saved;
    ctx->r[3] = 1;
}
