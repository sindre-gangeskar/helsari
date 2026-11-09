# Features

## Mouse look

Looking around with the mouse on foot is 1:1: raw input, with no acceleration or smoothing, updated every rendered
frame.

The game's logic runs at a fixed 15 Hz with an interpolated camera, so input through its look code reaches the screen
late and blended. With the mouse, the player's rotation is written every rendered frame instead, and the view is
corrected to match, so the camera, the character and the aim turn together. Turns the game makes itself, such as a
melee hit, still turn the view. Using a controller hands the look back to the game until the mouse moves again.

The viewmodel has its own transform at draw time: all of its parts follow one smoothed pose, which removes the 15 Hz
steps while the sights stay on target through the bob. **Options > Gameplay > Viewmodel Sway Multiplier** sets how much
sway there is (0.25x to 2x).

`HELSARI_MOUSE_SENS=<n>` sets the sensitivity, and `HELSARI_MOUSE_OFF=1` turns all of this off.

## Keyboard and prompts

**Options > Keybindings** rebinds every action. The on-screen button prompts follow the device you last used: your
bound keys on mouse and keyboard, Xbox labels on an Xbox controller, PlayStation icons on a DualSense.

In menus, the mouse cursor is free: hovering a row highlights it, a left click selects it and a right click goes back.

## PC options

All PC options are in the game's own options menu.

| Section | Option | Values |
|---|---|---|
| Graphics | Framerate limit | 30, 60, 120, unlimited |
| Graphics | Resolution | Sets the window size and the internal resolution (whole multiples of 720p) |
| Graphics | Window mode | Windowed, borderless |
| Graphics | V-sync | On, off |
| Graphics | Screen FX | Motion blur and depth of field, motion blur only, off |
| Graphics | Field of view | A slider from 73 (the original) to 110 degrees, horizontal; aiming keeps its original zoom |
| Gameplay | Viewmodel sway multiplier | 0.25x to 2x |
| Keybindings | Every action | Any key or mouse button |

Settings are saved to `settings.ini` next to the executable.
