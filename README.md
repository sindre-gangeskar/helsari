# Project Helsari - WIP
An unofficial Killzone 2 static recompilation project.  

An experimental static recompiler: the game's PowerPC (Cell PPU) machine code
is translated ahead of time into C, compiled with clang, and linked against a
runtime that stands in for the PS3 operating system and libraries.

**This repository contains no game code or data.** Everything game-derived is
generated on your machine, from your own **legally** obtained copy.

## Goals (Still to be verified if achieveable)
- PC Graphics options
- Unlocked framerates
- Native mouse and keyboard controls
- Game launcher that combines Killzone 2 and Killzone 3 for recomping and playing


## Status

| Area | State |
|---|---|
| PPU → C translation | All 2.9M reachable instructions (49k functions) translate; integer, FPU and VMX/AltiVec |
| Boot | crt0, TLS, libc init, ~15 engine threads (FIOS, audio, pad, SPURS handlers) |
| Kernel | mutex/cond/lwmutex/lwcond/semaphore/event flag/event queue/rwlock, threads |
| Files | `/dev_bdvd` mapped onto the dump, split `.666xx` archives joined transparently |
| SPU | Interpreter plus a static recompiler: SPURS task ELFs and job binaries in the EBOOT are compiled to C++ (`tools/recomp/spu.py`) and mapped when loaded into local store; Edge Zlib runs natively |
| GPU (RSX) | Direct3D 11 backend: vertex/fragment programs translated to HLSL, render-target cache, textures (swizzled, DXT, cube), blend/depth/stencil. Culling and shader branches not yet |
| Input | XInput controller mapped to DualShock 3 |
| Audio | Ports and timing events work; no sound output yet |

The game reaches its first screen — the "Welcome to Killzone 2" language
select — and renders it correctly. Gameplay will need the SPURS job chains
(SPU work) that are not run yet.

## Building

Requirements: Python 3.10+, CMake, Ninja, LLVM-MinGW (clang), and RPCS3 with
PS3 firmware installed (used once, to decrypt `EBOOT.BIN`).

```
winget install Python.Python.3.13 Kitware.CMake Ninja-build.Ninja MartinStorsjo.LLVM-MinGW.UCRT
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
      -DKZ2_DUMP="E:/PS3 Dumps/_BCES00081-[Killzone2]" -DRPCS3_EXE="E:/RPCS3/rpcs3.exe"
ninja -C build
build\kz2.exe
```

The configure step runs the recompiler (`tools/recomp`, ~10 s); the build
compiles ~154 MB of generated C (~20 s on 32 threads).

`kz2.exe --dump <dir>` overrides the dump location; saves and caches go to
`build/data/`.

### Debugging switches (environment variables)

| Variable | Effect |
|---|---|
| `KZ2_WATCHDOG=<sec>` | Periodically print every guest thread's current OS call, the most-called imports, and an RSX method histogram |
| `KZ2_TRACE_HLE=1` | Log the first 20 calls of every implemented import |
| `KZ2_TRACE_RSX=1` | Log the start of the RSX command stream |
| `KZ2_DUMP_FRAME=N` | Write every RSX method of frame N to `frame.txt` |
| `KZ2_DUMP_RT=N` | Save every colour render target at frame N as `rt_<address>.bmp` |
| `KZ2_DUMP_SHADERS=1` | Write generated HLSL to `shaders/` (failed compiles are always written) |
| `KZ2_LOG_DRAWS=N`, `KZ2_LOG_ATTRS=d` | Per-draw state for frame N; vertex attributes and textures of draw d |
| `KZ2_SKIP_DRAWS=a,b` | Skip draw indices (bisecting rendering bugs) |
| `KZ2_DUMP_FRAME=N` | Write every RSX method of frame N to `frame.txt` |
| `KZ2_DUMP_RT=N` | Save every colour render target at frame N as `rt_<address>.bmp` |
| `KZ2_DUMP_SHADERS=1` | Write generated HLSL to `shaders/` (failed compiles are always written) |
| `KZ2_LOG_DRAWS=N`, `KZ2_LOG_ATTRS=d` | Per-draw state for frame N; vertex attributes and textures of draw d |
| `KZ2_SKIP_DRAWS=a,b` | Skip draw indices (bisecting rendering bugs) |

## How it works

```
EBOOT.BIN ──rpcs3 --decrypt──▶ EBOOT.elf ──tools/recomp──▶ build/generated/*.c ──clang──▶ kz2.exe
                                                                                  ▲
                                                               runtime/ (OS + libraries)
```

**`tools/recomp`** (Python)
- `elf.py` — PS3 ELF: segments, `.opd` function descriptors, TLS, import stubs.
- `nid.py` — imports are identified by NID = SHA-1(name + salt)[0:4]. Names are
  recovered by hashing strings found in your `rpcs3.exe`; no database is shipped.
- `ppc.py` — PPU decoder (integer, branch, FPU, VMX).
- `analyze.py` — recursive-descent function discovery from every `.opd` entry and
  `bl` target; jump-table recovery (including bounds checks in predecessor blocks
  and TOC copies in other registers). Leaving a function's region becomes a tail
  call, so splitting functions is always safe.
- `codegen.py` — one C function per guest function; gotos for local branches,
  `musttail` for tail calls, C calls for `bl`.

**`runtime/`** (C++)
- Guest memory is a 4 GiB reservation committed on demand (vectored exception
  handler). Vector registers are stored byte-reversed so all lane sizes read
  natively.
- Imports are bound by writing fake function descriptors into the game's stub
  slots; calls land in `hle_*.cpp`. Unimplemented imports log once and return 0.
- `spurs.cpp` identifies SPU task ELFs by content and substitutes native code.

## What's next

1. **RSX renderer** — culling, shader control flow (IFE/LOOP/REP, VP branches),
   MSAA, 2D blit engines, performance (vertex fetch runs on the CPU).
2. **SPU** — Killzone 2 also runs SPURS job chains (skinning, culling, lighting).
   Either recompile SPU code the same way as the PPU, or replace jobs natively.
3. Audio output (the mixer already writes into the port buffer), save data.
4. The 21 unresolved indirect jumps are reported at runtime if ever reached.
