# Helsari: project rules

Helsari is a static recompiler and runtime. **This repository contains no code or data from any game.** Everything game-derived is generated on the user's machine from their own legally obtained copy. This is the project's most important rule, and it overrides convenience.

## Never commit or push

- Game executables or decrypted code: `EBOOT.BIN`, `.self`, `.sprx`, `.prx`, `.elf`
- Recompiled or generated output: `build/`, `generated/`, `spu_cache/`, recompiled PPU or SPU C or C++
- Game assets or archives: `.psarc`, `.core`, `.jsp` and SVML pages, textures, models, audio, video, fonts, localised text, dialogue
- Disassembly or decompiled listings, including pasted fragments and quoted page scripts
- Byte tables, shaders, or any other data copied or converted from the game, including base64 or hex dumps
- Captures and dumps: `pass_*.bmp`, `rt_*.bmp`, `tex_*.bin`, captured `.hlsl`, frame or run logs, screenshots of the game
- Saves and local settings: `CHECKPNT.DAT`, `build/data/`, `settings.ini`
- Sony SDK files or system modules, and encryption keys or firmware

## What is fine

- Our own recompiler, runtime and tools, written from scratch
- Re-implementations of system behaviour, in our own words and code
- Facts needed for interoperability, kept minimal: guest addresses, structure offsets, register meanings
- Short identifiers needed to hook something, such as a function or tag name
- Text and graphics we made ourselves

If unsure whether something is game-derived, leave it out and ask.

## Writing code and comments

- Describe behaviour in our own words. Do not paste the game's scripts, disassembly or strings into comments, commit messages, docs or issues.
- Load game data at run time from the user's dump, and patch it in memory only. Never write a modified game file into the repository.
- Keep debugging scripts and extracted data outside the repository.

## Before every commit

Stage files by name. Never use `git add .` or `git add -A`. Then review what is staged:

```sh
git diff --cached --stat
git diff --cached --numstat | awk '$1 == "-"'   # binary files: expect none, unless it is our own artwork
```

Stop and tell the user if anything is game-derived, even if they asked for the commit. If it has already been pushed, tell them immediately: removing it requires rewriting history.

## Workflow

- Commit and push only when the user asks, straight to `master`. Do not branch unless asked.
- PC options belong in the game's own options menu, never in a separate window.
