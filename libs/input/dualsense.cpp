// DualSense read directly over HID, so it works without Steam Input or
// DS4Windows. A background thread finds the controller (USB or Bluetooth),
// reads input reports and keeps the latest state as an XINPUT_GAMEPAD so
// the pad HLE can treat it like any other source.
#include "kernel.h"

#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <xinput.h>

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace {
std::mutex g_lock;
XINPUT_GAMEPAD g_state;
bool g_connected;

struct Device { HANDLE h; bool bluetooth; };

Device open_dualsense() {
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    Device found{INVALID_HANDLE_VALUE, false};
    if (set == INVALID_HANDLE_VALUE) return found;
    SP_DEVICE_INTERFACE_DATA ifd{sizeof(ifd)};
    for (DWORD i = 0; found.h == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        std::vector<uint8_t> buf(need);
        auto *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buf.data();
        det->cbSize = sizeof(*det);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES a{sizeof(a)};
        // 0x0ce6 = DualSense, 0x0df2 = DualSense Edge
        if (HidD_GetAttributes(h, &a) && a.VendorID == 0x054c && (a.ProductID == 0x0ce6 || a.ProductID == 0x0df2)) {
            // USB input reports are 64 bytes, Bluetooth ones 78.
            PHIDP_PREPARSED_DATA pp;
            HIDP_CAPS caps{};
            if (HidD_GetPreparsedData(h, &pp)) { HidP_GetCaps(pp, &caps); HidD_FreePreparsedData(pp); }
            found = {h, caps.InputReportByteLength > 64};
        } else {
            CloseHandle(h);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

SHORT stick(uint8_t v, bool invert) {
    int s = ((int)v - 128) * 256;
    if (invert) s = -s - 1;   // DualSense Y grows downward, XInput Y upward
    return (SHORT)std::clamp(s, -32768, 32767);
}

// p points at the left stick X byte. Full reports (USB 0x01, Bluetooth 0x31)
// put triggers before the buttons; the short Bluetooth 0x01 report, sent
// until the controller is switched to full mode, puts them after.
void parse(const uint8_t *p, bool short_report) {
    const uint8_t *btn = short_report ? p + 4 : p + 7;
    uint8_t l2 = short_report ? p[7] : p[4], r2 = short_report ? p[8] : p[5];
    XINPUT_GAMEPAD g{};
    g.sThumbLX = stick(p[0], false), g.sThumbLY = stick(p[1], true);
    g.sThumbRX = stick(p[2], false), g.sThumbRY = stick(p[3], true);
    g.bLeftTrigger = l2, g.bRightTrigger = r2;
    static const WORD hat[8] = {
        XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT,
        XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT,
    };
    if ((btn[0] & 15) < 8) g.wButtons |= hat[btn[0] & 15];
    static const struct { int byte, bit; WORD x; } map[] = {
        {0, 4, XINPUT_GAMEPAD_X}, {0, 5, XINPUT_GAMEPAD_A}, {0, 6, XINPUT_GAMEPAD_B}, {0, 7, XINPUT_GAMEPAD_Y},
        {1, 0, XINPUT_GAMEPAD_LEFT_SHOULDER}, {1, 1, XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {1, 4, XINPUT_GAMEPAD_BACK}, {1, 5, XINPUT_GAMEPAD_START},   // create, options
        {1, 6, XINPUT_GAMEPAD_LEFT_THUMB}, {1, 7, XINPUT_GAMEPAD_RIGHT_THUMB},
        {2, 1, XINPUT_GAMEPAD_BACK},                                 // touchpad click
    };
    for (auto &m : map) if (btn[m.byte] & (1 << m.bit)) g.wButtons |= m.x;
    std::lock_guard<std::mutex> l(g_lock);
    g_state = g;
}

void reader_thread() {
    for (;;) {
        Device d = open_dualsense();
        if (d.h == INVALID_HANDLE_VALUE) { Sleep(2000); continue; }
        LOG("pad", "DualSense connected (%s)", d.bluetooth ? "Bluetooth" : "USB");
        if (d.bluetooth) {
            // Reading the calibration feature report switches a Bluetooth
            // DualSense from the short 0x01 report to the full 0x31 one.
            uint8_t feat[64] = {0x05};
            HidD_GetFeature(d.h, feat, 41);
        }
        { std::lock_guard<std::mutex> l(g_lock); g_connected = true; }
        uint8_t r[128];
        DWORD n;
        while (ReadFile(d.h, r, sizeof(r), &n, nullptr)) {
            if (r[0] == 0x31 && n >= 12) parse(r + 2, false);
            else if (r[0] == 0x01 && n >= 10) parse(r + 1, d.bluetooth);
        }
        LOG("pad", "DualSense disconnected");
        CloseHandle(d.h);
        std::lock_guard<std::mutex> l(g_lock);
        g_connected = false, g_state = {};
    }
}
} // namespace

// Merge the DualSense state into g: buttons are OR'd, triggers and sticks
// take whichever source is pushed further.
void dualsense_merge(XINPUT_GAMEPAD &g) {
    static std::atomic<bool> started{false};
    if (!started.exchange(true)) std::thread(reader_thread).detach();
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_connected) return;
    g.wButtons |= g_state.wButtons;
    g.bLeftTrigger = std::max(g.bLeftTrigger, g_state.bLeftTrigger);
    g.bRightTrigger = std::max(g.bRightTrigger, g_state.bRightTrigger);
    auto pick = [](SHORT &a, SHORT b) { if (std::abs(b) > std::abs(a)) a = b; };
    pick(g.sThumbLX, g_state.sThumbLX), pick(g.sThumbLY, g_state.sThumbLY);
    pick(g.sThumbRX, g_state.sThumbRX), pick(g.sThumbRY, g_state.sThumbRY);
}
