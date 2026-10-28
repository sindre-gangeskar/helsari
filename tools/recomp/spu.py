"""SPU code in the EBOOT -> C++ (build/generated/spu).

SPU programs reach local store two ways: SPURS task ELFs, loaded by the
runtime (spurs_tasks.cpp), and job binaries that job managers DMA from the
EBOOT to a fixed LS address. Each such image is compiled as one "module":
its words are swept linearly and split into basic blocks, and every block
start becomes an entry point. Instruction semantics are not generated here:
each instruction is a call to the runtime's step<OP>(thread, insn, pc)
(runtime/src/spu/spu_ops.h) with constant operands, so recompiled code and
the interpreter execute identical semantics. Only control flow is compiled.

A generated chunk function `uint32_t f(SpuThread &t, uint32_t pc)` starts at
any of its block entries and returns the next pc whenever control leaves
what it knows (indirect branch to an unknown target, another chunk, an
instruction the interpreter must run). The runtime maps a module when its
image is loaded into LS and unmaps it when that LS range is written.
"""
import os
import re
import struct

OPS_HEADER = os.path.join(os.path.dirname(__file__), '..', '..', 'runtime', 'src', 'spu', 'spu_ops.h')
LS_MASK = 0x3fffc
CHUNK_INSNS = 3000            # instructions per generated function
JOB_LS_BASE = 0x4000          # job managers DMA job binaries here (observed for jm_post)
MAX_JOB_SIZE = 0x8000         # larger "jobs" between the ELFs are data

DIRECT = {'BR', 'BRA', 'BRSL', 'BRASL'}
COND = {'BRZ', 'BRNZ', 'BRHZ', 'BRHNZ'}
INDIRECT = {'BI', 'BISL', 'BIZ', 'BINZ', 'BIHZ', 'BIHNZ'}
TO_INTERP = {'STOP', 'STOPD', 'IRET', 'BISLED', 'INVALID'}   # the interpreter runs these
CHANNEL = {'RDCH', 'RCHCNT', 'WRCH'}
HALT = {'HEQ', 'HEQI', 'HGT', 'HGTI', 'HLGT', 'HLGTI'}


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


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def branch_target(op, x, pc):
    i16 = sext16((x >> 7) & 0xffff)
    return ((i16 << 2) if op in ('BRA', 'BRASL') else pc + (i16 << 2)) & LS_MASK


class Module:
    def __init__(self, name, src_ea, ls_base, code):
        self.name, self.src_ea, self.ls_base, self.code = name, src_ea, ls_base, code

    @property
    def end(self):
        return self.ls_base + len(self.code)


def find_modules(e):
    """SPU ELF text segments, and job binaries packed between them."""
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
        for k in range(phnum):
            p_type, off, vaddr, _, filesz, _, flags, _ = struct.unpack('>8I', e.read(ea + phoff + 32 * k, 32))
            if p_type == 1 and flags & 1 and filesz:
                mods.append(Module(f'elf_{ea:x}', ea + off, vaddr, e.read(ea + off, filesz & ~3)))
    # Job binaries sit between the ELFs: each ends with a 16-byte marker line
    # (ffffffff, then ffffffff in word 2 or 3), is zero-padded, and the next one
    # starts at the following 0x80-aligned non-zero line.
    if elves:
        def elf_end(ea):
            hdr = e.read(ea, 52)
            phoff, shoff = struct.unpack('>I', hdr[28:32])[0], struct.unpack('>I', hdr[32:36])[0]
            phnum, shnum = struct.unpack('>H', hdr[44:46])[0], struct.unpack('>H', hdr[48:50])[0]
            end = max(shoff + 40 * shnum, phoff + 32 * phnum)
            for k in range(phnum):
                _, off, _, _, filesz, _, _, _ = struct.unpack('>8I', e.read(ea + phoff + 32 * k, 32))
                end = max(end, off + filesz)
            return ea + end
        spans = [(ea, elf_end(ea)) for ea in elves]
        first, last = elves[0], elves[-1]
        region = e.read(first, last - first)

        def line(a):
            return region[a - first:a - first + 16]

        def is_marker(l):
            return l[:4] == b'\xff' * 4 and (l[8:12] == b'\xff' * 4 or l[12:16] == b'\xff' * 4)

        starts = []
        for a in range(first + 0x80, last, 0x80):
            if any(s <= a < t for s, t in spans) or line(a) == bytes(16) or line(a - 16) != bytes(16):
                continue
            b = a - 16
            while b > first and line(b) == bytes(16):
                b -= 16
            if is_marker(line(b)) and not is_marker(line(a)):
                starts.append(a)
        for a in starts:
            b = a
            while b < last and not is_marker(line(b)) and not any(s <= b < t for s, t in spans):
                b += 16
            if b - a <= MAX_JOB_SIZE:
                mods.append(Module(f'job_{a:x}', a, JOB_LS_BASE, e.read(a, b - a)))
    return mods


def gen_module(dec, m, mi):
    """C++ for one module; returns (source text, [(pc, function name)], [function names])."""
    n = len(m.code) // 4
    words = struct.unpack(f'>{n}I', m.code[:4 * n])
    pcs = [m.ls_base + 4 * k for k in range(n)]
    ops = [dec.op(x) for x in words]
    starts = {m.ls_base}
    for k, (x, op, pc) in enumerate(zip(words, ops, pcs)):
        if op in DIRECT or op in COND:
            t = branch_target(op, x, pc)
            if m.ls_base <= t < m.end:
                starts.add(t)
        if op in DIRECT or op in COND or op in INDIRECT or op in TO_INTERP or op in CHANNEL:
            if pc + 4 < m.end:
                starts.add(pc + 4)

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
        fn = f'spu_m{mi}_c{ci}'
        fns.append(fn)
        labels = sorted(s for s in starts if lo <= s < hi)
        entries += [(s, fn) for s in labels]

        def jump(t, pc):
            if lo <= t < hi and t in starts:
                if t <= pc:   # backward: let pending interrupts in, as the interpreter would
                    return f'if (t.interrupts) {{ next = 0x{t:x}; goto redispatch; }} goto L_{t:x};'
                return f'goto L_{t:x};'
            return f'return 0x{t:x};'

        o = [f'uint32_t {fn}(SpuThread &t, uint32_t next) {{',
             '    const uint32_t epoch = t.code_epoch;',
             '    goto dispatch;',
             'redispatch:',
             '    if (t.interrupts && t.poll_interrupt(next)) return next;',
             '    if (t.code_epoch != epoch) return next;',
             'dispatch:',
             '    switch (next) {']
        o += [f'    case 0x{s:x}: goto L_{s:x};' for s in labels]
        o += ['    default: return next;', '    }']
        for k in range((lo - m.ls_base) // 4, (hi - m.ls_base) // 4):
            x, op, pc = words[k], ops[k], pcs[k]
            nxt = (pc + 4) & LS_MASK
            if pc in starts:
                o.append(f'L_{pc:x}:')
            s = f'step<{op}>(t, 0x{x:08x}u, 0x{pc:x}u)'
            if op in TO_INTERP:
                o.append(f'    t.interp_once = true; return 0x{pc:x};')
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
                o.append(f'    t.pc = 0x{pc:x}; {s}; if (!t.running || t.code_epoch != epoch) return 0x{nxt:x};')
            elif op in HALT:
                o.append(f'    {s}; if (!t.running) return 0x{nxt:x};')
            else:
                o.append(f'    {s};')
        o.append(f'    return 0x{hi & LS_MASK:x};')
        o.append('}')
        out.append('\n'.join(o))
    return '\n\n'.join(out) + '\n', entries, fns


def generate(e, out, log):
    """Write build/generated/spu/*.cpp; returns the list of source files."""
    dec = Decoder()
    mods = find_modules(e)
    spu_dir = os.path.join(out, 'spu')
    os.makedirs(spu_dir, exist_ok=True)
    for f in os.listdir(spu_dir):
        os.remove(os.path.join(spu_dir, f))
    files, table = [], []
    prelude = '#include "spu/spu_ops.h"\n\nnamespace spu {\n\n'
    for mi, m in enumerate(mods):
        src, entries, fns = gen_module(dec, m, mi)
        p = os.path.join(spu_dir, f'spu_m{mi:02d}.cpp')
        with open(p, 'w') as c:
            c.write(f'// {m.name}: {len(m.code)} bytes from 0x{m.src_ea:x}, LS 0x{m.ls_base:x}\n')
            c.write(prelude + src + '\n} // namespace spu\n')
        files.append(p)
        table.append((m, mi, entries, fns))
    p = os.path.join(spu_dir, 'spu_modules.cpp')
    with open(p, 'w') as c:
        c.write('#include "spu/spu.h"\n\nnamespace spu {\n\n')
        for m, mi, entries, fns in table:
            for fn in fns:
                c.write(f'uint32_t {fn}(SpuThread &, uint32_t);\n')
            c.write(f'static const NativeEntry m{mi}_entries[] = {{\n')
            c.write(''.join(f'    {{0x{pc:x}, {fn}}},\n' for pc, fn in entries))
            c.write('};\n')
        c.write('extern const NativeModule g_native_modules[] = {\n')
        for m, mi, entries, fns in table:
            c.write(f'    {{"{m.name}", 0x{m.src_ea:x}, 0x{m.ls_base:x}, 0x{len(m.code):x}, m{mi}_entries, {len(entries)}}},\n')
        c.write('};\n')
        c.write(f'extern const int g_native_module_count = {len(table)};\n\n}} // namespace spu\n')
    files.append(p)
    ninsn = sum(len(m.code) // 4 for m in mods)
    log(f'SPU: {len(mods)} modules ({sum(1 for m in mods if m.name.startswith("elf"))} ELF segments), '
        f'{ninsn} instructions')
    return files
