// Keyboard and mouse bindings. See keybinds.cpp.
#pragma once

#include <windows.h>

#include <xinput.h>

#include <string>

constexpr int KEYBIND_COUNT = 14;

const char *keybind_id(int action);       // its name in settings.ini
int keybind_key(int action);              // its virtual key
void keybind_set(int action, int vk);     // from settings.ini
std::string keybind_row(int action);      // "FIRE: LEFT MOUSE", for the options menu
void keybind_begin_capture(int action);   // the next key or mouse button goes to this action
bool keybind_take_refresh();              // a key was just taken: the menu has to show it
void keybind_message(UINT m, WPARAM w, LPARAM l); // window thread, for the key being captured
void keybind_apply(XINPUT_GAMEPAD &g);    // pad read: the keyboard and mouse as a pad
int keybind_generation();                 // changes whenever a key or the prompt set does
enum PromptSet { PROMPTS_PC, PROMPTS_XBOX, PROMPTS_PLAYSTATION };
PromptSet keybind_prompts();              // which prompts to show: the last input device's
void keybind_note_pad(bool dualsense);    // a controller was used
void keybind_note_square_prompt();        // the HUD is showing its square button (a pick up prompt)
void keybind_paint_hud(unsigned char *rgba, int width, int height); // the bound keys over the HUD atlas's buttons
