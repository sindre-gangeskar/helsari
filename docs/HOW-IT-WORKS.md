# How it works

```
EBOOT.BIN ──rpcs3 --decrypt──▶ EBOOT.elf ──tools/recomp──▶ build/generated/ ──clang──▶ helsari2.exe
                                                                                ▲
                                                         runtime/, libs/ and port/
```

## Subsystems

| Part | How it is handled |
| :--- | :--- |
| **PPU (CPU)** | Statically recompiled: all reachable code (2.9M instructions, 49k functions) becomes native C, including full FPU and VMX/AltiVec support. |
| **SPU** | Statically recompiled from the EBOOT and from SPU code captured at run time; float, shuffle and rotate instructions use host SIMD. |
| **SPURS** | Job-manager workloads run on up to 5 host threads each; task sets, events and mailboxes are emulated. |
| **RSX (GPU)** | Translated to Direct3D 11: shaders are recompiled to HLSL (cached on disk), with a render-target cache and vertex decoding on the GPU. |
| **OS layer** | Threads, synchronisation, memory maps, save data and the file system (`/dev_bdvd` mapped onto your dump). |
| **Input** | XInput and DualSense controllers as a DualShock 3; raw mouse look on foot. |
| **Audio** | The game's `cellAudio` ports mixed into 7.1 surround and played through WASAPI. |

## The recompiler: `tools/recomp` (Python)

- `elf.py`: PS3 ELF segments, `.opd` function descriptors, TLS, import stubs.
- `nid.py`: imports are identified by NID = SHA-1(name + salt)[0:4]. Names are recovered by hashing strings found in
  your `rpcs3.exe`; no database is shipped.
- `ppc.py`, `analyze.py`, `codegen.py`: PPU decoding, function discovery (from every `.opd` entry and `bl` target, with
  jump-table recovery), and one C function per guest function.
- `overrides.py`: the game functions Helsari replaces. The generated original is renamed `f_XXXXXXXX_guest`, so a
  replacement can still call it.
- `spu.py`, `spu_cache.py`: SPU modules (task ELFs, job binaries, captured code) to C++, sharing instruction semantics
  with the interpreter.

## Folder layout

The layout follows the one used by the ps3recomp toolkit, which Helsari studies as a reference; all code here is
Helsari's own.

| Folder | What it holds |
|---|---|
| `runtime/` | Process start (`main.cpp`) and the core (`core.cpp`) |
| `runtime/ppu/` | Guest threads and fibers |
| `runtime/spu/` | The SPU interpreter, native SPU code, DMA and code capture |
| `runtime/memory/` | Guest memory: a 4 GiB reservation committed on demand |
| `runtime/syscalls/` | Kernel calls: threads, synchronisation, timers, files |
| `runtime/diag/` | Debug hooks and the profiler |
| `runtime/include/` | Headers shared by the runtime and the generated code |
| `libs/video/` | The graphics command interface and the renderer (`rsx/`) |
| `libs/spurs/` | SPURS: the kernel, task sets and job chains |
| `libs/input/` | Pads, DualSense and audio output |
| `libs/system/` | System utilities and save data |
| `libs/codec/` | Decompression |
| `port/` | Everything specific to this game: mouse look, key bindings, the PC options and the menu patch |

Everything outside `port/` is meant to work for any PS3 game. Imports are bound by writing function descriptors into
the game's stub slots, so calls land in the matching library in `libs/`.
