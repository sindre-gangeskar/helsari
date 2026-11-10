<p align="center" width="100%">
      <img src=".github/assets/images/helsari_logo.webp" alt="Helsari Logo" width="80%"/>
</p>
<p align="center" width="100%"><strong>WIP - Not yet ready for play</strong>
</p>
<p align="center" width="100%">
      <img src=".github/assets/images/helsari_note.webp" alt="Helsari Note" width="80%"/>
</p>
<p align="center" width="100%">
      <strong>Unofficial Static Recompilation project for Killzone 2 and Killzone 3</strong>
</p>

Helsari turns the game's PlayStation 3 code into a native Windows program. It is not an emulator: the game's own code
is translated to C ahead of time and compiled on your PC, from your own copy. Nothing from the game is included here.

## Features

- **Mouse and keyboard**: 1:1 raw mouse look, rebindable keys, on-screen prompts that follow the device you use
- **PC options** in the game's own menu: framerate limit, resolution, window mode, V-sync, screen effects, field of
  view, viewmodel sway, intro movie skip
- **Unlocked framerate**, with the game's logic still running at its own speed
- **Higher internal resolution**, up to 4K

Details: [docs/FEATURES.md](docs/FEATURES.md).

## What you need

- Your own disc dump of the game (European release, BCES00081, is the one tested)
- The game's 1.29 update, from your own copy. If you have played the game in RPCS3, it is already in RPCS3's
  `dev_hdd0/game/<title id>` folder, and the build finds it there
- Windows 10 or 11, 64-bit, with a CPU that supports AVX2
- The build tools listed in [docs/BUILDING.md](docs/BUILDING.md)

## Quick start

```
cmake -B build -G Ninja -DHELSARI_DUMP="<path_to_ps3_dump>" -DRPCS3_EXE="<path_to_rpcs3.exe>"
ninja -C build
build\helsari2.exe
```

The full steps, including compiler setup: [docs/BUILDING.md](docs/BUILDING.md).

## Documentation

| Document                             | What it covers                                    |
| ------------------------------------ | ------------------------------------------------- |
| [Building](docs/BUILDING.md)         | Requirements, build steps, where files go         |
| [Features](docs/FEATURES.md)         | Mouse look, controls, PC options                  |
| [How it works](docs/HOW-IT-WORKS.md) | The recompiler, the runtime and the folder layout |
| [Debugging](docs/DEBUGGING.md)       | Debug keys and environment variables              |
| [Roadmap](docs/ROADMAP.md)           | What is next                                      |

## Legal

Helsari is a fan project. It is not affiliated with or endorsed by Sony Interactive Entertainment or Guerrilla Games.
It contains no code or data from any game; everything game-derived is generated on your machine from your own legally
obtained copy.
