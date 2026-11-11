# Building

## Requirements

- Python 3.10 or later
- CMake and Ninja
- Visual Studio Build Tools with its Clang component
- RPCS3 with PS3 firmware installed, used once to decrypt the update's `EBOOT.BIN`
- The game's 1.29 update. Helsari runs 1.29 only: its executable is the one recompiled, and the game reads the
  update's patches itself
- A CPU with AVX2, to run the game

Microsoft's Clang is signed, so Windows Smart App Control allows it. The build still has LLVM-MinGW settings, but Smart
App Control can block that unsigned compiler, and they are untested since the switch.

## Install the tools

```
winget install Python.Python.3.13 Kitware.CMake Ninja-build.Ninja
winget install Microsoft.VisualStudio.BuildTools --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.Windows11SDK.26100"
```

## Build

```
set LLVM=C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/Llvm/x64/bin
cmake -B build -G Ninja "-DCMAKE_C_COMPILER=%LLVM%/clang.exe" "-DCMAKE_CXX_COMPILER=%LLVM%/clang++.exe" ^
      -DHELSARI_DUMP="<path_to_ps3_dump>" -DRPCS3_EXE="<path_to_rpcs3.exe>"
ninja -C build
build\helsari2.exe
```

The build looks for the update next to `rpcs3.exe`, in `dev_hdd0/game/<title id>`. If it is elsewhere, add
`-DHELSARI_UPDATE="<path_to_update_folder>"`; `helsari2.exe --update <dir>` overrides it at run time. The update
stays where it is: the game reads it there.

Configuring runs the recompiler (`tools/recomp`); the build then compiles the generated code. Changing any file in
`tools/recomp` regenerates all of it, which takes several minutes.

Configuring also downloads AMD FidelityFX (a pinned SDK release, checked by SHA-256) into
`build/fidelityfx`: the headers of its API and AMD's signed upscaler DLLs, which the DX12 renderer uses. The DLLs
and their licence (`FidelityFX-license.md`) are copied next to the executable. Offline, the build goes on without
them.

## Where files go

| Path | What it holds |
|---|---|
| `build/generated/` | The translated game code. Never commit or share it |
| `build/spu_cache/` | SPU code the game loads at run time, captured and compiled natively on the next build |
| `build/data/` | Saves and the virtual hard drive |
| `build/settings.ini` | Your PC options and key bindings |

The first runs are slower, until the captured SPU code has been compiled. So are entry points of compiled programs
that are only reached through computed jumps (`spu_cache/entries.txt`).

`helsari2.exe --dump <dir>` overrides the dump location.
