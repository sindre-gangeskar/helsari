"""Build-time pipeline: user's own game dump -> generated C sources.

    python -m recomp --dump "<dump dir>" --rpcs3 <rpcs3.exe> --out <build/generated>

Nothing produced here is meant to be committed: it is all derived from the
user's own copy of the game.
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import time

from . import analyze, codegen, elf, nid, spu
from .overrides import OVERRIDES

KNOWN_TITLES = {'BCES00081': 'EU', 'BCUS98116': 'US',
                'BCJS30022': 'JP', 'BCAS20071': 'Asia'}

CODE_LO_SECTION = 1           # .init: first code section after the null section
CHUNK_INSNS = 20000           # instructions per generated .c file


def log(*a):
    print('[recomp]', *a, flush=True)


def read_sfo(path):
    d = open(path, 'rb').read()
    magic, ver, key_tab, data_tab, n = struct.unpack('<IIIII', d[:20])
    out = {}
    for k in range(n):
        ko, fmt, ln, mx, do = struct.unpack('<HHIII', d[20 + 16 * k:36 + 16 * k])
        key = d[key_tab + ko:d.index(b'\0', key_tab + ko)].decode()
        raw = d[data_tab + do:data_tab + do + ln]
        out[key] = struct.unpack('<I', raw)[0] if fmt == 0x0404 else raw.rstrip(b'\0').decode('utf-8', 'replace')
    return out


def decrypt(update, rpcs3, out):
    eboot = os.path.join(update, 'USRDIR', 'EBOOT.BIN')
    if not os.path.isfile(eboot):
        sys.exit(f'EBOOT.BIN not found under {update}\\USRDIR')
    work = os.path.join(out, 'decrypt')
    os.makedirs(work, exist_ok=True)
    src_hash = hashlib.sha1(open(eboot, 'rb').read()).hexdigest()
    stamp = os.path.join(work, 'source.sha1')
    elf_path = os.path.join(work, 'EBOOT.elf')
    if os.path.isfile(elf_path) and os.path.isfile(stamp) and open(stamp).read() == src_hash:
        log('decrypted EBOOT is up to date')
        return elf_path
    if not rpcs3 or not os.path.isfile(rpcs3):
        sys.exit('RPCS3 is needed once to decrypt EBOOT.BIN (pass --rpcs3 path\\to\\rpcs3.exe; '
                 'its firmware must be installed)')
    copy = os.path.join(work, 'EBOOT.BIN')
    shutil.copyfile(eboot, copy)
    log('decrypting with RPCS3 ...')
    subprocess.run([rpcs3, '--no-gui', '--decrypt', copy], check=False, timeout=600)
    if not os.path.isfile(elf_path):
        sys.exit('RPCS3 did not produce EBOOT.elf - is the PS3 firmware installed in RPCS3?')
    open(stamp, 'w').write(src_hash)
    return elf_path


def c_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def generate(e, out, rpcs3):
    t0 = time.time()
    code_lo = e.sections[CODE_LO_SECTION].addr
    stubs = e.imports()
    code_hi = _code_end(e)
    log(f'code range {code_lo:#x}-{code_hi:#x}')
    prog, funcs = analyze.analyze(e, code_lo, code_hi)
    ninsn = sum(len(f.insns) for f in funcs.values())
    log(f'{len(funcs)} functions, {ninsn} instructions, '
        f'{sum(len(f.jumptables) for f in funcs.values())} jump tables, '
        f'{len(prog.unresolved_jumps)} unresolved indirect jumps ({time.time() - t0:.1f}s)')

    gen = codegen.Gen(prog, funcs)
    ppu_dir = os.path.join(out, 'ppu')
    os.makedirs(ppu_dir, exist_ok=True)
    # Outputs are rewritten only when their content changes (spu.out_file), so
    # regenerating after an unrelated recompiler change recompiles nothing.
    out_file = spu.out_file

    order = sorted(funcs)
    with out_file(os.path.join(out, 'ppu_funcs.h')) as h:
        h.write('#pragma once\n#include "ppu.h"\n')
        for a in order:
            h.write(f'void f_{a:08x}(PPU_CTX);\n')
            if a in OVERRIDES:
                h.write(f'void f_{a:08x}_guest(PPU_CTX);  /* native override: {OVERRIDES[a]} */\n')
    missing = [a for a in OVERRIDES if a not in funcs]
    if missing:
        sys.exit(f'overrides name addresses that are not functions: {[hex(a) for a in missing]}')

    files = []
    chunk, size, idx = [], 0, 0

    def flush():
        nonlocal chunk, size, idx
        if not chunk:
            return
        p = os.path.join(ppu_dir, f'ppu_{idx:04d}.c')
        with out_file(p) as c:
            c.write('#include "ppu_funcs.h"\n\n')
            for a in chunk:
                c.write(gen.function(funcs[a], f'f_{a:08x}_guest' if a in OVERRIDES else None))
                c.write('\n\n')
        files.append(p)
        chunk, size, idx = [], 0, idx + 1

    for a in order:
        chunk.append(a)
        size += len(funcs[a].insns)
        if size >= CHUNK_INSNS:
            flush()
    flush()
    spu.remove_stale(ppu_dir, files)
    log(f'wrote {len(files)} C files ({time.time() - t0:.1f}s)')
    if gen.missing:
        log('untranslated instructions (emitted as ppu_unimpl):', dict(gen.missing))

    with out_file(os.path.join(out, 'ppu_table.c')) as c:
        c.write('#include "ppu_funcs.h"\n#include "runtime.h"\n\n')
        c.write(f'const uint32_t g_func_count = {len(order)};\n')
        c.write('const FuncEntry g_funcs[] = {\n')
        for a in order:
            c.write(f'  {{0x{a:x}, f_{a:08x}}},\n')
        c.write('};\n')

    # imports: names come from hashing candidate strings, never from a shipped DB
    names = {}
    cands = set()
    if rpcs3 and os.path.isfile(rpcs3):
        cands |= nid.names_from_binary(rpcs3)
    names = nid.resolve([i.nid for i in stubs], cands)
    with out_file(os.path.join(out, 'imports.c')) as c:
        c.write('#include "runtime.h"\n\n')
        c.write(f'const uint32_t g_import_count = {len(stubs)};\n')
        c.write('const ImportEntry g_imports[] = {\n')
        for i in stubs:
            c.write(f'  {{{c_str(i.module)}, {c_str(names.get(i.nid, ""))}, 0x{i.nid:08x}u, 0x{i.stub_addr:x}u}},\n')
        c.write('};\n')
    log(f'{len(stubs)} imports, {len(names)} names resolved')

    pp = e.process_param()
    tls = e.tls()
    opd_entry = e.entry
    func, toc = struct.unpack('>II', e.read(opd_entry, 8))
    with out_file(os.path.join(out, 'game_config.h')) as c:
        c.write('#pragma once\n')
        c.write(f'#define GAME_ENTRY_OPD 0x{opd_entry:x}u\n#define GAME_ENTRY 0x{func:x}u\n#define GAME_TOC 0x{toc:x}u\n')
        c.write(f'#define GAME_TLS_ADDR 0x{tls[0]:x}u\n#define GAME_TLS_FILESZ 0x{tls[1]:x}u\n#define GAME_TLS_MEMSZ 0x{tls[2]:x}u\n')
        c.write(f'#define GAME_PRIMARY_PRIO {pp["primary_prio"]}\n#define GAME_PRIMARY_STACK 0x{pp["primary_stacksize"]:x}u\n')
        c.write(f'#define GAME_MALLOC_PAGESIZE 0x{pp["malloc_pagesize"]:x}u\n#define GAME_SDK_VERSION 0x{pp["sdk_version"]:x}u\n')
    with out_file(os.path.join(out, 'sources.txt')) as s:
        for p in files + [os.path.join(out, 'ppu_table.c'), os.path.join(out, 'imports.c')]:
            s.write(p.replace('\\', '/') + '\n')
    spu_files = spu.generate(e, out, log, spu.read_extra_entries(os.path.join(out, '..', 'spu_cache')))
    with out_file(os.path.join(out, 'spu_sources.txt')) as s:
        for p in spu_files:
            s.write(p.replace('\\', '/') + '\n')
    log(f'done in {time.time() - t0:.1f}s')


def _code_end(e):
    """End of the import-stub text: the code segment's sections are .init,
    .text, .fini, .sceStub.text, then read-only data. We stop after the
    stub section, identified as the section whose words are the stub
    pattern `li r12,0; oris r12,r12,hi; lwz r12,lo(r12)`."""
    for s in e.sections:
        if s.size >= 12 and s.addr:
            w = struct.unpack('>3I', e.read(s.addr, 12))
            if w[0] == 0x39800000 and (w[1] >> 16) == 0x658c and (w[2] >> 16) == 0x818c:
                return s.addr + s.size
    sys.exit('could not find the import stub section')


def main():
    ap = argparse.ArgumentParser(prog='recomp')
    ap.add_argument('--dump', required=True, help='Game disc dump folder (contains PS3_GAME)')
    ap.add_argument('--update', required=True, help='Game update 1.29 folder (dev_hdd0/game/<title id>)')
    ap.add_argument('--rpcs3', help='rpcs3.exe, used to decrypt EBOOT.BIN and to recover function names')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    sfo = os.path.join(a.dump, 'PS3_GAME', 'PARAM.SFO')
    if not os.path.isfile(sfo):
        sys.exit(f'{a.dump} does not look like a PS3 disc dump (no PS3_GAME/PARAM.SFO)')
    title = read_sfo(sfo).get('TITLE_ID', '?')
    log(f'title {title}: {KNOWN_TITLES.get(title, "not a supported title")}')
    if title not in KNOWN_TITLES:
        sys.exit('not a supported dump')
    if title != 'BCES00081':
        log('warning: only BCES00081 has been tested')
    # Helsari runs the game's 1.29 update: its executable is the one recompiled, and the game reads the
    # update's patches itself.
    usfo = os.path.join(a.update, 'PARAM.SFO')
    if not os.path.isfile(usfo):
        sys.exit(f'{a.update} does not look like the game update (no PARAM.SFO)')
    u = read_sfo(usfo)
    if u.get('TITLE_ID') != title or u.get('APP_VER') != '01.29':
        sys.exit(f'{a.update} is {u.get("TITLE_ID")} version {u.get("APP_VER")}; Helsari needs {title} version 01.29')
    log(f'update {u.get("APP_VER")}')
    os.makedirs(a.out, exist_ok=True)
    elf_path = decrypt(a.update, a.rpcs3, a.out)
    generate(elf.load(elf_path), a.out, a.rpcs3)


if __name__ == '__main__':
    main()
