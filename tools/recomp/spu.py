"""SPU code in the EBOOT -> C++ (build/generated/spu).

SPU programs reach local store two ways: SPURS task ELFs, loaded by the
runtime (spurs_tasks.cpp), and job binaries that job managers DMA from the
EBOOT to a fixed LS address. Each such image is compiled as one "module":
its words are swept linearly and split into basic blocks, and every block
start becomes an entry point. Instruction semantics are not generated here:
each instruction is a call to the runtime's step<OP>(thread, insn, pc)
(runtime/spu/spu_ops.h) with constant operands, so recompiled code and
the interpreter execute identical semantics. Only control flow is compiled.

A generated chunk function `uint32_t f(SpuThread &t, V *g, uint32_t pc)` starts
at any of its block entries. When control leaves what it knows (another chunk,
an indirect branch elsewhere) it tail-calls the compiled code there if the run
loop would, and otherwise returns the next pc (an instruction the interpreter
must run, a channel stop, a pending interrupt, a trap). The runtime maps a module when its
image is loaded into LS and unmaps it when that LS range is written.
"""
import contextlib
import io
import os
import re
import struct

OPS_HEADER = os.path.join(os.path.dirname(__file__), '..', '..', 'runtime', 'spu', 'spu_ops.h')
LS_MASK = 0x3fffc
CHUNK_INSNS = 4000             # instructions per generated function (larger: slow, memory-hungry compiles)
JOB_LS_BASE = 0x4000          # job managers DMA job binaries here (observed for jm_post)

DIRECT = {'BR', 'BRA', 'BRSL', 'BRASL'}
COND = {'BRZ', 'BRNZ', 'BRHZ', 'BRHNZ'}
INDIRECT = {'BI', 'BISL', 'BIZ', 'BINZ', 'BIHZ', 'BIHNZ'}
TO_INTERP = {'STOP', 'STOPD', 'IRET', 'BISLED', 'INVALID'}   # the interpreter runs these
CHANNEL = {'RDCH', 'RCHCNT', 'WRCH'}
BLOCK_ENDS = {'BR', 'BRA', 'BI', 'BRSL', 'BRASL', 'BISL'} | TO_INTERP   # control does not fall through
HALT = {'HEQ', 'HEQI', 'HGT', 'HGTI', 'HLGT', 'HLGTI'}
# Instructions that write no register (everything else writes rt; see reg_fields).
NO_WRITE = {'STQD', 'STQX', 'STQA', 'STQR', 'BR', 'BRA', 'BRZ', 'BRNZ', 'BRHZ', 'BRHNZ', 'BI', 'BIZ', 'BINZ', 'BIHZ',
            'BIHNZ', 'WRCH', 'STOP', 'STOPD', 'LNOP', 'NOP', 'SYNC', 'DSYNC', 'IRET', 'HBR', 'HBRA', 'HBRR', 'MTSPR',
            'INVALID'} | HALT


def reg_fields(op, fmt, x):
    """(registers an instruction may read or write, register it writes or None).
    Over-approximates reads: every 7-bit register field of the encoding."""
    rt = (x >> 21) & 0x7f if fmt == 'Q' else x & 0x7f
    used = {x & 0x7f, (x >> 7) & 0x7f, (x >> 14) & 0x7f}
    if fmt == 'Q':
        used.add((x >> 21) & 0x7f)
    return used, (None if op in NO_WRITE else rt)


def load_ops():
    """The X-macro opcode list from spu_ops.h: [(enum, mnemonic, bits, opcode, fmt)]."""
    text = open(OPS_HEADER, encoding='utf-8').read()
    ops = [('INVALID', '?', 0, 0, 'N')]
    for e, name, bits, code, fmt in re.findall(r"X\((\w+), \"([^\"]*)\", (\d+), (0x[0-9a-f]+|0), '(.)'\)", text):
        if e != 'INVALID':
            ops.append((e, name, int(bits), int(code, 16), fmt))
    return ops


class Decoder:
    def __init__(self):
        self.ops = load_ops()
        self.table = [0] * 2048
        for bits in (4, 7, 8, 9, 10, 11):   # narrower opcodes first so exact 11-bit entries win
            for i, (_, _, b, code, _) in enumerate(self.ops):
                if b == bits:
                    span = 1 << (11 - bits)
                    for k in range(span):
                        self.table[(code << (11 - bits)) | k] = i

    def op(self, x):
        return self.ops[self.table[x >> 21]][0]

    def fmt(self, x):
        return self.ops[self.table[x >> 21]][4]


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def branch_target(op, x, pc):
    i16 = sext16((x >> 7) & 0xffff)
    return ((i16 << 2) if op in ('BRA', 'BRASL') else pc + (i16 << 2)) & LS_MASK


class Module:
    """An SPU image executed at LS ls_base. protect: per quadword, whether it
    holds code (None: all); only those quadwords are compiled. embed: the runtime
    compares LS with the image bytes compiled in, not with the EBOOT."""
    def __init__(self, name, src_ea, ls_base, code, protect=None, embed=False):
        self.name, self.src_ea, self.ls_base, self.code = name, src_ea, ls_base, code
        self.protect, self.embed = protect, embed

    def is_code(self, pc):
        return self.protect is None or self.protect[(pc - self.ls_base) >> 4]

    def word_checked(self, pc):
        """Is this word part of what the runtime compares with LS? Captures record executed
        words: a word of a code quadword that never ran may differ between loads (data next
        to code), so its compiled form could be stale - it goes through the interpreter."""
        if self.protect is None:
            return True
        v = self.protect[(pc - self.ls_base) >> 4]
        return v is True or bool((int(v) >> ((pc >> 2) & 3)) & 1)

    @property
    def end(self):
        return self.ls_base + len(self.code)


def find_modules(e):
    """SPU ELF text segments, job binaries (found by their header), and the policy module."""
    mods = []
    seg = next(s for s in e.segments if s.type == 1)          # the read-only image: code and rodata
    data, base = e.data[seg.offset:seg.offset + seg.filesz], seg.vaddr
    # SPU ELFs: 7f ELF, 32-bit big-endian, e_machine 23
    elves = []
    i = data.find(b'\x7fELF')
    while i >= 0:
        if data[i + 4] == 1 and data[i + 5] == 2 and struct.unpack('>H', data[i + 18:i + 20])[0] == 23:
            elves.append(base + i)
        i = data.find(b'\x7fELF', i + 1)
    for ea in elves:
        hdr = e.read(ea, 52)
        phoff, phnum = struct.unpack('>I', hdr[28:32])[0], struct.unpack('>H', hdr[44:46])[0]
        # Each executable segment whole, not just its declared code sections: programs run code
        # outside them. Data among the code is safe to compile as instructions (never run), and the
        # runtime tolerates the program changing it (stale quadwords, spu_native.cpp); zero quadwords
        # are data to it.
        for k in range(phnum):
            p_type, off, vaddr, _, filesz, _, flags, _ = struct.unpack('>8I', e.read(ea + phoff + 32 * k, 32))
            if p_type == 1 and flags & 1 and filesz:
                size = filesz & ~3
                mods.append(Module(f'elf_{ea:x}', ea + off, vaddr, e.read(ea + off, size)))
    # Job binaries: a 48-byte header, with the constant 0xc0dec0de at +0x20. +0x10: offset of the
    # program in the binary (0x30 or 0x80), +0x14: its size, +0x28: the LS address it runs at, or 0:
    # 0x8400 if the flag word at +0x2c has its top bit set, else 0x4000; the flag's low byte is 5.
    # Found by the header alone, wherever the job sits: the EBOOT has no other index of them.
    have = {(m.src_ea, m.ls_base) for m in mods}
    heads = []
    i = data.find(JOB_MAGIC)
    while i >= 0:
        h = i - 0x20
        if h >= 0 and h % 16 == 0:
            w = struct.unpack('>12I', data[h:h + 48])
            if w[11] & 0xff == 5 and 0 < w[5] < 0x40000 and w[4] < 0x40000:
                heads.append((base + h, w))
        i = data.find(JOB_MAGIC, i + 1)
    # A job ends where the next image starts (the header's size field can only shorten that: some
    # jobs state a size well past their binary).
    bounds = sorted([ea for ea, _ in heads] + elves + [ea for _, ea, _, _ in KNOWN_IMAGES] + [base + len(data)])
    for ea, w in heads:
        ls = w[10] if w[10] else (0x8400 if w[11] & 0x80000000 else JOB_LS_BASE)
        nxt = next(b for b in bounds if b > ea)
        size = min(w[4] + w[5] if w[4] in (0x30, 0x80) else nxt - ea, nxt - ea, 0x40000 - ls)
        size = (size + 15) & ~15
        if (ea, ls) not in have:
            mods.append(Module(f'job_{ea:x}', ea, ls, e.read(ea, size)))
            have.add((ea, ls))
    # The job managers' policy module, loaded at 0xa00 by the SPURS kernel (spurs_kernel.cpp): no
    # header to find it by. A fact about this game version (BCES00081, update 1.29).
    for name, ea, ls, size in KNOWN_IMAGES:
        if (ea, ls) not in have and e.read(ea, 16) != bytes(16):
            mods.append(Module(f'{name}_{ea:x}', ea, ls, e.read(ea, size)))
    return mods


JOB_MAGIC = bytes.fromhex('c0dec0de')
KNOWN_IMAGES = [
    ('pm', 0xda2c80, 0xa00, 0x2e80),     # EBOOT address, LS address, size
]


def gen_module(dec, m, prefix, extra_starts=()):
    """C++ for one module; returns (source text, [(pc, function name)], [function names]).
    Functions are named <prefix>_c<chunk>. extra_starts: entry points seen only at run time
    (computed jumps; spu_cache/entries.txt)."""
    n = len(m.code) // 4
    words = struct.unpack(f'>{n}I', m.code[:4 * n])
    pcs = [m.ls_base + 4 * k for k in range(n)]
    ops = [dec.op(x) for x in words]
    starts = {m.ls_base}
    for k, (x, op, pc) in enumerate(zip(words, ops, pcs)):
        if not m.is_code(pc):
            continue
        if not m.word_checked(pc):
            if pc + 4 < m.end:
                starts.add(pc + 4)
            continue
        if op in DIRECT or op in COND:
            t = branch_target(op, x, pc)
            if m.ls_base <= t < m.end:
                starts.add(t)
        if op in DIRECT or op in COND or op in INDIRECT or op in TO_INTERP or op in CHANNEL:
            if pc + 4 < m.end:
                starts.add(pc + 4)
    # Code entered from data (fall-through) starts a block too.
    for pc in range(m.ls_base, m.end, 16):
        if m.is_code(pc) and pc > m.ls_base and not m.is_code(pc - 16):
            starts.add(pc)
    starts |= {s for s in extra_starts if m.ls_base <= s < m.end}
    starts = {s for s in starts if m.is_code(s)}

    # Quadwords the runtime treats as code (spu_native.cpp is_code): a capture's executed words; an
    # EBOOT image's non-zero quadwords (zero words decode as STOP, which the interpreter runs).
    def runtime_code(qpc):
        if m.protect is not None:
            return bool(m.protect[(qpc - m.ls_base) >> 4])
        off = qpc - m.ls_base
        return any(m.code[off:off + 16])

    def block_quads(k):
        """Quadwords (absolute LS index) of the block starting at instruction k: up to and including
        its first unconditional transfer, or until the next block start."""
        qs = []
        while k < n:
            q = pcs[k] >> 4
            if (not qs or qs[-1] != q) and runtime_code(q << 4):
                qs.append(q)
            if ops[k] in BLOCK_ENDS or (k + 1 < n and pcs[k + 1] in starts):
                break
            k += 1
        return qs

    # chunks: runs of whole blocks, about CHUNK_INSNS instructions each
    chunks, cur = [], m.ls_base
    for k in range(n):
        pc = pcs[k]
        if pc in starts and pc - cur >= 4 * CHUNK_INSNS:
            chunks.append((cur, pc))
            cur = pc
    chunks.append((cur, m.end))

    out, entries, fns = [], [], []
    for ci, (lo, hi) in enumerate(chunks):
        fn = f'{prefix}_c{ci}'
        fns.append(fn)
        labels = sorted(s for s in starts if lo <= s < hi)
        entries += [(s, fn) for s in labels]

        def jump(t, pc):
            if lo <= t < hi and t in starts and m.is_code(t):
                if t <= pc:   # backward: let pending interrupts in, as the interpreter would
                    return f'if (t.interrupts) {{ next = 0x{t:x}; goto redispatch; }} goto L_{t:x};'
                return f'goto L_{t:x};'
            return f'{{ ret = 0x{t:x}; goto chain; }}'

        # SPU registers: the thread's own register file, through a no-alias parameter. Nothing else
        # touches t.gpr while compiled code runs, so the compiler may keep registers in host registers
        # across LS stores, as with a local copy - without copying the registers in and out on every
        # entry, which cost most of the time of call-heavy tasks (a chunk is left at every call into
        # another chunk: ~73,000 times per run of one 7 ms task).
        o = [f'uint32_t {fn}(SpuThread &t, V *__restrict g, uint32_t next) {{',
             '    const uint32_t epoch = t.code_epoch;',
             '    uint32_t ret;']
        o += ['    goto dispatch;',
              'redispatch:',
              '    if (t.interrupts && t.poll_interrupt(next)) { ret = next; goto out; }',
              '    if (t.code_epoch != epoch) { ret = next; goto out; }',
              'dispatch:',
              '    switch (next) {']
        o += [f'    case 0x{s:x}: goto L_{s:x};' for s in labels]
        o += ['    default: ret = next; goto chain;', '    }']
        for k in range((lo - m.ls_base) // 4, (hi - m.ls_base) // 4):
            x, op, pc = words[k], ops[k], pcs[k]
            nxt = (pc + 4) & LS_MASK
            if not m.is_code(pc):
                continue
            if pc in starts:
                o.append(f'L_{pc:x}:')
                # The block's quadwords must still hold what was compiled: the program may have
                # written them (runtime code_q: STALE, see spu.h). A stale one goes to stale_block.
                for q in block_quads(k):
                    o.append(f'    if (t.code_q[0x{q:x}] != 1) {{ ret = t.stale_block(0x{pc:x}, 0x{q:x}); goto out; }}')
            s = f'step_r<{op}>(t, g, 0x{x:08x}u, 0x{pc:x}u)'
            if op in TO_INTERP or not m.word_checked(pc):
                o.append(f'    t.interp_once = true; ret = 0x{pc:x}; goto out;')
            elif op in DIRECT:
                o.append(f'    {s}; {jump(branch_target(op, x, pc), pc)}')
            elif op in COND:
                o.append(f'    if ({s} != 0x{nxt:x}u) {{ {jump(branch_target(op, x, pc), pc)} }}')
            elif op in INDIRECT:
                if op in ('BI', 'BISL'):
                    o.append(f'    next = {s}; goto redispatch;')
                else:
                    o.append(f'    {{ uint32_t n = {s}; if (n != 0x{nxt:x}u) {{ next = n; goto redispatch; }} }}')
            elif op in CHANNEL:
                o.append(f'    t.pc = 0x{pc:x}; {s}; if (!t.running || t.code_epoch != epoch) {{ ret = 0x{nxt:x}; goto out; }}')
            elif op in HALT:
                o.append(f'    {s}; if (!t.running) {{ ret = 0x{nxt:x}; goto out; }}')
            else:
                o.append(f'    {s};')
            if m.ls_base <= nxt < m.end and not m.is_code(nxt):
                o.append(f'    ret = 0x{nxt:x}; goto out;')   # falls into data: not compiled
        o.append(f'    ret = 0x{hi & LS_MASK:x};')
        # Somewhere this chunk does not hold: straight on into the compiled code there (a tail call,
        # so chains of chunks use no stack), under the run loop's own conditions; else back to it.
        o.append('chain:')
        # With interrupts on, a pending event goes back to the run loop (which enters the handler).
        o.append('    if (t.interrupts && t.chain_interrupt(ret)) return ret;')
        o.append('    if (t.running && t.code_epoch == epoch && !t.traps[(ret & 0x3ffff) >> 2])')
        o.append('        if (NativeFn f = t.native[(ret & 0x3ffff) >> 2]) [[clang::musttail]] return f(t, g, ret & 0x3ffff);')
        o.append('out:')
        o.append('    return ret;')
        o.append('}')
        out.append('\n'.join(o))
    return '\n\n'.join(out) + '\n', entries, fns




def write_if_changed(path, text):
    """Rewrite path only if its content changes, so the build recompiles only what changed."""
    if os.path.isfile(path) and open(path, encoding='utf-8').read() == text:
        return
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)


@contextlib.contextmanager
def out_file(path):
    """`with out_file(p) as f:` - like open(p, 'w'), but only rewrites p if its content changes."""
    buf = io.StringIO()
    yield buf
    write_if_changed(path, buf.getvalue())


def remove_stale(directory, keep):
    """Delete files in directory that this generation did not produce."""
    keep = {os.path.normcase(os.path.abspath(p)) for p in keep}
    for f in os.listdir(directory):
        p = os.path.join(directory, f)
        if os.path.normcase(os.path.abspath(p)) not in keep:
            os.remove(p)


def byte_array(data):
    return ',\n'.join(','.join(f'0x{b:02x}' for b in data[i:i + 32]) for i in range(0, len(data), 32))


def module_table(table, symbol):
    """C++ lines for a NativeModule table; table = [(module, tag, entries, function names)].
    Returns (lines, row count); the caller appends the count definition."""
    out = ['#include "spu/spu.h"', '', 'namespace spu {', '']
    rows = []
    for m, tag, entries, fns in table:
        out += [f'uint32_t {fn}(SpuThread &, V *__restrict, uint32_t);' for fn in fns]   # restrict is in the mangled name
        out.append(f'static const NativeEntry {tag}_entries[] = {{')
        out += [f'    {{0x{pc:x}, {fn}}},' for pc, fn in entries]
        out.append('};')
        image = protect = 'nullptr'
        if m.embed:
            out.append(f'static const uint8_t {tag}_image[] = {{\n{byte_array(m.code)}}};')
            image = f'{tag}_image'
        if m.protect is not None:
            out.append(f'static const uint8_t {tag}_protect[] = {{{",".join(str(15 if b is True else int(b)) for b in m.protect)}}};')
            protect = f'{tag}_protect'
        rows.append(f'    {{"{m.name}", 0x{m.src_ea:x}, 0x{m.ls_base:x}, 0x{len(m.code):x}, {tag}_entries, '
                    f'{len(entries)}, {image}, {protect}}},')
    out.append(f'extern const NativeModule {symbol}[] = {{')
    out += rows or ['    {},']
    out.append('};')
    return out, len(rows)


PRELUDE = '#include "spu/spu_ops.h"\n\nnamespace spu {\n\n'


def read_extra_entries(cache_dir):
    """spu_cache/entries.txt (spu_capture.cpp): "<module> <pc hex>" per line -> {module: {pc}}."""
    extra = {}
    path = os.path.join(cache_dir, 'entries.txt')
    if os.path.isfile(path):
        for line in open(path, encoding='utf-8'):
            p = line.split()
            if len(p) == 2:
                extra.setdefault(p[0], set()).add(int(p[1], 16))
    return extra


def generate(e, out, log, extra=None):
    """Write build/generated/spu/*.cpp; returns the list of source files."""
    extra = extra or {}
    dec = Decoder()
    mods = find_modules(e)
    spu_dir = os.path.join(out, 'spu')
    os.makedirs(spu_dir, exist_ok=True)
    files, table = [], []
    for mi, m in enumerate(mods):
        src, entries, fns = gen_module(dec, m, f'spu_m{mi}', extra.get(m.name, ()))
        if not entries:
            continue
        p = os.path.join(spu_dir, f'spu_m{mi:02d}.cpp')
        with out_file(p) as c:
            c.write(f'// {m.name}: {len(m.code)} bytes from 0x{m.src_ea:x}, LS 0x{m.ls_base:x}\n')
            c.write(PRELUDE + src + '\n} // namespace spu\n')
        files.append(p)
        table.append((m, f'm{mi}', entries, fns))
    p = os.path.join(spu_dir, 'spu_modules.cpp')
    lines, n = module_table(table, 'g_native_modules')
    lines += [f'extern const int g_native_module_count = {n};', '', '} // namespace spu', '']
    with out_file(p) as c:
        c.write('\n'.join(lines))
    files.append(p)
    remove_stale(spu_dir, files)
    ninsn = sum(len(m.code) // 4 for m in mods)
    log(f'SPU: {len(mods)} modules ({sum(1 for m in mods if m.name.startswith("elf"))} ELF segments), '
        f'{ninsn} instructions')
    return files


RT_BUCKETS = 16                       # fixed file count, so the build's source list never changes


def rt_sources(out):
    rt_dir = os.path.join(out, 'spu_rt')
    return [os.path.join(rt_dir, f'spu_rt_{i:02d}.cpp') for i in range(RT_BUCKETS)] + \
        [os.path.join(rt_dir, 'spu_rt_modules.cpp')]


def read_cache_files(cache_dir):
    """spu_cache/*.bin written by the runtime (spu_capture.cpp), file format:
    "SPC2", ls_base, size, mask (one byte per quadword: executed), image.
    "SPC1" files have one mask byte per 128-byte line. Yields (path, ls_base,
    per-quadword mask, image); incomplete files are skipped."""
    if not os.path.isdir(cache_dir):
        return
    for f in sorted(os.listdir(cache_dir)):
        if not re.fullmatch(r'[0-9a-f]+_[0-9a-f]{16}\.bin', f):
            continue
        path = os.path.join(cache_dir, f)
        d = open(path, 'rb').read()
        if len(d) < 12:
            continue
        magic, ls_base, size = struct.unpack('<4sII', d[:12])
        grain = {b'SPC1': 128, b'SPC2': 16}.get(magic)        # bytes per mask entry
        nmask = (size + grain - 1) // grain if grain else 0
        if not grain or len(d) < 12 + nmask + size:
            continue
        # per quadword: a bit per executed word (0x10 | bits); older files: nonzero = all four
        mask = [word_bits(d[12 + q * 16 // grain]) for q in range((size + 15) // 16)]
        yield path, ls_base, mask, d[12 + nmask:12 + nmask + size]


def word_bits(v):
    return 0 if not v else (v & 0xf) if v & 0x10 else 0xf


def capture_hash(ls_base, image):
    """The runtime's file name hash (spu_capture.cpp image_hash): FNV-1a seeded with ls_base."""
    h = 1469598103934665603 ^ ls_base
    for b in image:
        h = ((h ^ b) * 1099511628211) & 0xffffffffffffffff
    return h


def write_capture(cache_dir, ls_base, mask, image):
    path = os.path.join(cache_dir, f'{ls_base:05x}_{capture_hash(ls_base, image):016x}.bin')
    with open(path, 'wb') as f:
        f.write(struct.pack('<4sII', b'SPC2', ls_base, len(image)) + bytes(0x10 | b if b else 0 for b in mask) + image)


def read_cache(cache_dir):
    """Captured images -> [Module], named after their file's hash."""
    mods = []
    for path, ls_base, mask, image in read_cache_files(cache_dir):
        h = os.path.basename(path)[-20:-4]
        mods.append(Module(f'rt_{ls_base:x}_{h}', 0, ls_base, image[:len(image) & ~3], mask, embed=True))
    return mods


def generate_cache(cache_dir, out, log):
    """Runtime-captured SPU code -> build/generated/spu_rt/ (spu_rt_00..15.cpp, spu_rt_modules.cpp)."""
    dec = Decoder()
    os.makedirs(os.path.join(out, 'spu_rt'), exist_ok=True)
    buckets = [[] for _ in range(RT_BUCKETS)]
    table = []
    extra = read_extra_entries(cache_dir)
    for m in read_cache(cache_dir):
        h = m.name.rsplit('_', 1)[1]
        src, entries, fns = gen_module(dec, m, f'spu_rt_{h}', extra.get(m.name, ()))
        if not entries:
            continue
        buckets[int(h, 16) % RT_BUCKETS].append(f'// {m.name}: {len(m.code)} bytes at LS 0x{m.ls_base:x}\n' + src)
        table.append((m, f'rt_{h}', entries, fns))
    files = rt_sources(out)
    for i, b in enumerate(buckets):
        write_if_changed(files[i], PRELUDE + '\n'.join(b) + '\n} // namespace spu\n')
    lines, n = module_table(table, 'g_rt_modules')
    lines += [f'extern const int g_rt_module_count = {n};', '', '} // namespace spu', '']
    write_if_changed(files[-1], '\n'.join(lines))
    compiled = sum(sum(bin(b).count('1') for b in m.protect) for m, _, _, _ in table)
    log(f'SPU cache: {n} captured images, {compiled} instructions compiled')
    return files
