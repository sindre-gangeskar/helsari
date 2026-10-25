"""Function discovery for the PPU executable.

Recursive descent from every known entry point (.opd descriptors, the ELF
entry, and every `bl` target found along the way). Only reachable
instructions are translated, so data embedded in .text (jump tables,
literal pools) is never mistaken for code.

A function is "the code reachable from its entry without passing through
another entry". Leaving that region (a branch or fall-through into another
entry) becomes a tail call, which keeps splitting always semantically safe.
"""
import struct
from dataclasses import dataclass, field

from . import ppc

SPR_CTR = 9


@dataclass
class Function:
    addr: int
    insns: dict = field(default_factory=dict)       # addr -> (mnemonic, Insn)
    labels: set = field(default_factory=set)        # addrs that need a C label
    jumptables: dict = field(default_factory=dict)  # bcctr addr -> [targets]


class Program:
    def __init__(self, elf, code_lo, code_hi):
        self.elf = elf
        self.lo, self.hi = code_lo, code_hi
        raw = elf.read(code_lo, code_hi - code_lo)
        self.words = struct.unpack('>%dI' % (len(raw) // 4), raw)
        self._dec = {}
        self.toc = elf.opd()[0][1]
        self.unresolved_jumps = []

    def in_code(self, a):
        return self.lo <= a < self.hi and not a & 3

    def word(self, a):
        return self.words[(a - self.lo) >> 2]

    def decode(self, a):
        d = self._dec.get(a)
        if d is None:
            d = self._dec[a] = ppc.decode(self.word(a))
        return d

    # -- jump tables -------------------------------------------------------
    def branch_sources(self, t):
        """Addresses of direct (non-linking) branches to t. Built on first use."""
        if not hasattr(self, '_srcs'):
            self._srcs = {}
            for k, w in enumerate(self.words):
                op = w >> 26
                if op not in (16, 18) or w & 1:
                    continue
                a = self.lo + 4 * k
                m, i = self.decode(a)
                d = i.li if op == 18 else i.bd
                tgt = (d if i.aa else a + d) & 0xffffffff
                self._srcs.setdefault(tgt, []).append(a)
        return self._srcs.get(t, [])

    def _find_back(self, at, pred, depth=3, limit=32):
        """Search backwards from `at` for an instruction matching pred,
        continuing into the branch sources of a block that has no
        fall-through predecessor."""
        a = at - 4
        for _ in range(limit):
            if a < self.lo:
                return None
            m, i = self.decode(a)
            if m in ('b', 'bclr') or (m == 'bcctr' and not i.lk):
                break  # a+4 is a block start reached only by branches
            if pred(m, i):
                return i
            a -= 4
        if depth:
            # Any address of this block may be the branch target (blocks are
            # often preceded by alignment nops).
            for blk in range(a + 4, at, 4):
                for s in self.branch_sources(blk):
                    r = self._find_back(s + 4, pred, depth - 1, limit)
                    if r:
                        return r
        return None

    def _reg_value(self, at, reg):
        """Static value of a base register: r2 is the TOC; otherwise look for
        the nearest preceding `mr reg,r2` or `lwz reg,d(r2)` (a pointer kept
        in a callee-saved register for the whole function)."""
        if reg == 2:
            return self.toc
        for a in range(at - 4, max(self.lo, at - 4 * 4096) - 1, -4):
            m, i = self.decode(a)
            if m is None or i.rd != reg:
                continue
            if m in ('ld', 'lwz') and i.ra == 1:
                continue  # epilogue restore of a callee-saved register, not its definition
            if m == 'or' and i.ra == 2 and i.rb == 2:
                return self.toc
            if m == 'lwz' and i.ra == 2:
                p = (self.toc + i.simm) & 0xffffffff
                return self.elf.u32(p) if self.elf.seg_for(p) else None
            if m in ('or', 'addi', 'addis', 'ld', 'lwz', 'rldicl', 'rlwinm', 'add'):
                return None  # some other definition: give up
        return None

    def jumptable(self, at):
        """Resolve `lwz base,d(toc); cmpli N; lwzx/lwax; [extsw]; add; mtctr; bctr`.

        The TOC may have been copied out of r2, so any `lwz rX,d(rY)` whose
        rX feeds the final add is tried with the TOC as rY's value.
        """
        add = self._find_back(at, lambda m, i: m == 'add', depth=0, limit=8)
        if add is None:
            return None
        regs = {add.ra, add.rb}
        ld = self._find_back(at, lambda m, i: m == 'lwz' and i.rd in regs, depth=2)
        if ld is None:
            return None
        # An index clamped by masking, e.g. `rlwinm r9,r3,2,22,29` (index*4 &
        # 0x3fc), needs no bounds check; the mask only gives an upper bound, so
        # the table is read until an entry leaves the code. A plain `slwi`
        # (mb = 0) masks nothing, and the bound comes from the cmpli instead.
        idx = self._find_back(at, lambda m, i: m == 'rlwinm' and i.sh32 == 2 and i.me32 == 29
                              and 16 <= i.mb32 <= i.me32, depth=0, limit=8)
        if idx is not None:
            # mask covers bits mb..29 (bit 0 = MSB): entries = (mask >> 2) + 1
            bound, exact = ((0xffffffff >> idx.mb32) & ~3 & 0xffffffff) // 4 + 1, False
        else:
            cmp = self._find_back(at, lambda m, i: m == 'cmpli', depth=3)
            if cmp is None:
                return None
            bound, exact = cmp.uimm + 1, True
        ptr = self._reg_value(at, ld.ra)
        if ptr is None or self.elf.seg_for((ptr + ld.simm) & 0xffffffff) is None:
            return None
        base = self.elf.u32((ptr + ld.simm) & 0xffffffff)
        if bound > 4096 or self.elf.seg_for(base) is None:
            return None
        targets = []
        for k in range(bound):
            if self.elf.seg_for(base + 4 * k) is None:
                break
            off = struct.unpack('>i', self.elf.read(base + 4 * k, 4))[0]
            t = (base + off) & 0xffffffff
            # Without an exact bound, also stop at targets far from the jump:
            # they belong to the data after the table, not to this switch.
            if not self.in_code(t) or (not exact and abs(t - at) > 0x10000):
                if exact:
                    return None
                break
            targets.append(t)
        if exact and len(targets) != bound:
            return None
        return targets or None

    # -- discovery ---------------------------------------------------------
    def discover(self, roots):
        entries = set(a for a in roots if self.in_code(a))
        funcs = {}
        work = sorted(entries)
        while work:
            new = []
            for f in work:
                if f in funcs:
                    continue
                fn = self._walk(f, entries, new)
                funcs[f] = fn
            work = [a for a in new if a not in funcs]
            for a in work:
                entries.add(a)
        # A later-discovered entry may sit inside an earlier function's body.
        # Re-walk those so they tail-call it instead of duplicating its code.
        self.entries = entries
        stale = [f for f, fn in funcs.items() if any(a in entries and a != f for a in fn.insns)]
        for f in stale:
            funcs[f] = self._walk(f, entries, [])
        return funcs

    def _walk(self, f, entries, new_entries):
        fn = Function(f)
        stack = [f]
        while stack:
            a = stack.pop()
            if a in fn.insns:
                continue
            if not self.in_code(a):
                continue
            if a != f and a in entries:
                continue  # reached by fall-through/branch: emitted as tail call
            m, i = self.decode(a)
            fn.insns[a] = (m, i)
            if m is None:
                continue  # emitted as a trap
            nxt = a + 4

            def goto(t):
                if t == f or t not in entries:
                    fn.labels.add(t)
                    stack.append(t)

            if m == 'b':
                t = (i.li if i.aa else a + i.li) & 0xffffffff
                if i.lk:
                    if t != nxt and self.in_code(t) and t not in entries:
                        new_entries.append(t)
                    stack.append(nxt)
                else:
                    goto(t)
                continue
            if m == 'bc':
                t = (i.bd if i.aa else a + i.bd) & 0xffffffff
                if i.lk:
                    if t != nxt and self.in_code(t) and t not in entries:
                        new_entries.append(t)
                else:
                    goto(t)
                stack.append(nxt)
                continue
            if m == 'bclr':
                if i.lk or (i.bo & 0x14) != 0x14:
                    stack.append(nxt)
                continue
            if m == 'bcctr':
                if i.lk or (i.bo & 0x14) != 0x14:
                    stack.append(nxt)
                    continue
                jt = self.jumptable(a)
                if jt:
                    fn.jumptables[a] = jt
                    for t in jt:
                        goto(t)
                else:
                    self.unresolved_jumps.append(a)
                continue
            stack.append(nxt)
        return fn


def analyze(elf, code_lo, code_hi):
    prog = Program(elf, code_lo, code_hi)
    roots = [fa for fa, _ in elf.opd()]
    funcs = prog.discover(roots)
    return prog, funcs
