"""Translate analysed PPU functions into C.

Every guest function becomes `void f_XXXXXXXX(PPUContext *ctx)`. Branches
inside the function are gotos; leaving it is a guaranteed tail call; `bl` is
a C call; `blr` is `return`. All semantics helpers live in runtime/include.
"""
from collections import Counter

R = lambda n: f'ctx->r[{n}]'
F = lambda n: f'ctx->f[{n}]'
V = lambda n: f'ctx->v[{n}]'


def ra0(i):
    return R(i.ra) if i.ra else '0'


def mask64(b, e):
    def m(b, e):
        return ((1 << (64 - b)) - 1) ^ ((1 << (63 - e)) - 1)
    return m(b, e) if b <= e else m(b, 63) | m(0, e)


def hx(v):
    return f'0x{v:x}ull'


def s16(v):
    return f'(int64_t){v}'


class Gen:
    def __init__(self, prog, funcs):
        self.prog = prog
        self.funcs = funcs
        self.missing = Counter()

    # -- branch helpers ---------------------------------------------------
    def bc_cond(self, bo, bi, out):
        """Emits CTR decrement if needed, returns a C condition string."""
        parts = []
        if not bo & 4:
            out.append('ctx->ctr--;')
            parts.append('ctx->ctr == 0' if bo & 2 else 'ctx->ctr != 0')
        if not bo & 16:
            parts.append(f'CRBIT({bi})' if bo & 8 else f'!CRBIT({bi})')
        return ' && '.join(f'({p})' for p in parts) or '1'

    def jump(self, fn, t):
        if t in fn.labels:
            return f'goto L_{t:08x};'
        if t in self.funcs:
            return f'TAIL(f_{t:08x});'
        return f'ctx->jt = 0x{t:x}; TAIL(ppu_dispatch);'

    def call(self, t):
        if t in self.funcs:
            return f'f_{t:08x}(ctx);'
        return f'ppu_call(ctx, 0x{t:x});'

    # -- one function -----------------------------------------------------
    def function(self, fn, name=None):
        out = [f'void {name or f"f_{fn.addr:08x}"}(PPU_CTX) {{']
        addrs = sorted(fn.insns)
        # labels: branch targets plus fall-through targets that aren't adjacent
        labels = set(fn.labels)
        for k, a in enumerate(addrs):
            nxt = addrs[k + 1] if k + 1 < len(addrs) else None
            if nxt != a + 4 and (a + 4) in fn.insns:
                labels.add(a + 4)
        fn.labels = labels
        if fn.addr != addrs[0]:
            out.append(f'goto L_{fn.addr:08x};')
            labels.add(fn.addr)
        for k, a in enumerate(addrs):
            if a in labels:
                out.append(f'L_{a:08x}:;')
            m, i = fn.insns[a]
            body = []
            falls = self.insn(fn, a, m, i, body)
            out.extend('  ' + s for s in body)
            if falls:
                nxt = addrs[k + 1] if k + 1 < len(addrs) else None
                if nxt != a + 4:
                    out.append('  ' + self.jump(fn, a + 4) if (a + 4) in fn.insns or (a + 4) in self.funcs
                               else f'  ppu_unimpl(ctx, 0x{a + 4:x}, 0); return;')
        out.append('}')
        return '\n'.join(out)

    def insn(self, fn, a, m, i, o):
        """Append C for one instruction to o; return True if it falls through."""
        if m is None:
            o.append(f'ppu_unimpl(ctx, 0x{a:x}, 0x{i.w:08x}); return;')
            return False
        h = getattr(self, 'i_' + m.replace('.', '_'), None)
        if h is not None:
            r = h(fn, a, i, o)
            return True if r is None else r
        e = SIMPLE.get(m)
        if e is not None:
            o.append(e(i))
            return True
        self.missing[m] += 1
        o.append(f'ppu_unimpl(ctx, 0x{a:x}, 0x{i.w:08x}); return;')
        return False

    # -- branches ---------------------------------------------------------
    def i_b(self, fn, a, i, o):
        t = (i.li if i.aa else a + i.li) & 0xffffffff
        if i.lk:
            o.append(f'ctx->lr = 0x{a + 4:x};')
            if t != a + 4:
                o.append(self.call(t))
            return True
        o.append(self.jump(fn, t))
        return False

    def i_bc(self, fn, a, i, o):
        t = (i.bd if i.aa else a + i.bd) & 0xffffffff
        c = self.bc_cond(i.bo, i.bi, o)
        if i.lk:
            o.append(f'if ({c}) {{ ctx->lr = 0x{a + 4:x}; {self.call(t)} }}')
        else:
            o.append(f'if ({c}) {{ {self.jump(fn, t)} }}')
        return True

    def i_bclr(self, fn, a, i, o):
        c = self.bc_cond(i.bo, i.bi, o)
        if i.lk:
            o.append(f'if ({c}) {{ uint32_t t = (uint32_t)ctx->lr; ctx->lr = 0x{a + 4:x}; ppu_call(ctx, t); }}')
            return True
        if c == '1':
            o.append('return;')
            return False
        o.append(f'if ({c}) return;')
        return True

    def i_bcctr(self, fn, a, i, o):
        c = self.bc_cond(i.bo | 4, i.bi, o)  # bcctr never decrements CTR
        if i.lk:
            o.append(f'if ({c}) {{ ctx->lr = 0x{a + 4:x}; ppu_call(ctx, (uint32_t)ctx->ctr); }}')
            return True
        targets = fn.jumptables.get(a)
        if targets is None:  # unresolved: try every label of this function
            targets = sorted(fn.labels)
        cases = ' '.join(f'case 0x{t:x}: goto L_{t:08x};' for t in sorted(set(targets)) if t in fn.labels)
        tail = f'ctx->jt = (uint32_t)ctx->ctr; TAIL(ppu_dispatch);'
        if c == '1':
            o.append(f'switch ((uint32_t)ctx->ctr) {{ {cases} }}')
            o.append(tail)
            return False
        o.append(f'if ({c}) {{ switch ((uint32_t)ctx->ctr) {{ {cases} }} {tail} }}')
        return True

    def i_sc(self, fn, a, i, o):
        o.append('ppu_syscall(ctx);')

    # -- traps --------------------------------------------------------------
    def _trap(self, a, to, x, y, o, w):
        if to == 0:
            return
        cs = []
        sx, ux = (f'(int32_t){x}', f'(uint32_t){x}') if w == 32 else (f'(int64_t){x}', f'(uint64_t){x}')
        sy, uy = (f'(int32_t){y}', f'(uint32_t){y}') if w == 32 else (f'(int64_t){y}', f'(uint64_t){y}')
        if to & 16: cs.append(f'{sx} < {sy}')
        if to & 8: cs.append(f'{sx} > {sy}')
        if to & 4: cs.append(f'{sx} == {sy}')
        if to & 2: cs.append(f'{ux} < {uy}')
        if to & 1: cs.append(f'{ux} > {uy}')
        o.append(f'if ({" || ".join(cs)}) ppu_trap(ctx, 0x{a:x});')

    def i_tw(self, fn, a, i, o): self._trap(a, i.to, R(i.ra), R(i.rb), o, 32)
    def i_td(self, fn, a, i, o): self._trap(a, i.to, R(i.ra), R(i.rb), o, 64)
    def i_twi(self, fn, a, i, o): self._trap(a, i.to, R(i.ra), s16(i.simm), o, 32)
    def i_tdi(self, fn, a, i, o): self._trap(a, i.to, R(i.ra), s16(i.simm), o, 64)

    # -- special registers --------------------------------------------------
    def i_mfspr(self, fn, a, i, o):
        src = {1: 'get_xer(ctx)', 8: 'ctx->lr', 9: 'ctx->ctr', 256: 'ctx->vrsave',
               268: 'ppu_timebase()', 269: '(ppu_timebase() >> 32)'}.get(i.spr)
        if src is None:
            self.missing[f'mfspr{i.spr}'] += 1
            src = '0'
        o.append(f'{R(i.rd)} = {src};')

    def i_mtspr(self, fn, a, i, o):
        s = R(i.rs)
        d = {1: f'set_xer(ctx, {s});', 8: f'ctx->lr = {s};', 9: f'ctx->ctr = {s};',
             256: f'ctx->vrsave = (uint32_t){s};'}.get(i.spr)
        if d is None:
            self.missing[f'mtspr{i.spr}'] += 1
            d = ';'
        o.append(d)

    def i_mftb(self, fn, a, i, o):
        o.append(f'{R(i.rd)} = {"ppu_timebase() >> 32" if i.spr == 269 else "ppu_timebase()"};')

    def i_mfcr(self, fn, a, i, o): o.append(f'{R(i.rd)} = get_cr(ctx);')

    def i_mtcrf(self, fn, a, i, o): o.append(f'set_crf(ctx, 0x{i.crm:x}, (uint32_t){R(i.rs)});')

    def i_mcrf(self, fn, a, i, o): o.append(f'ctx->cr[{i.crfd}] = ctx->cr[{i.crfs}];')

    def i_mfmsr(self, fn, a, i, o): o.append(f'{R(i.rd)} = 0x8000000000000000ull;')

    # -- CR logic ------------------------------------------------------------
    def _crop(self, i, o, expr):
        o.append(f'SETCRBIT({i.rd}, ({expr.format(a=f"CRBIT({i.ra})", b=f"CRBIT({i.rb})")}) & 1);')

    def i_crand(self, fn, a, i, o): self._crop(i, o, '{a} & {b}')
    def i_cror(self, fn, a, i, o): self._crop(i, o, '{a} | {b}')
    def i_crxor(self, fn, a, i, o): self._crop(i, o, '{a} ^ {b}')
    def i_crnand(self, fn, a, i, o): self._crop(i, o, '~({a} & {b})')
    def i_crnor(self, fn, a, i, o): self._crop(i, o, '~({a} | {b})')
    def i_creqv(self, fn, a, i, o): self._crop(i, o, '~({a} ^ {b})')
    def i_crandc(self, fn, a, i, o): self._crop(i, o, '{a} & ~{b}')
    def i_crorc(self, fn, a, i, o): self._crop(i, o, '{a} | ~{b}')

    # -- compares ------------------------------------------------------------
    def i_cmp(self, fn, a, i, o):
        t = 'int64_t' if i.l10 else 'int32_t'
        o.append(f'ctx->cr[{i.crfd}] = cmp_s(({t}){R(i.ra)}, ({t}){R(i.rb)}) | ctx->xer_so;')

    def i_cmpl(self, fn, a, i, o):
        t = 'uint64_t' if i.l10 else 'uint32_t'
        o.append(f'ctx->cr[{i.crfd}] = cmp_u(({t}){R(i.ra)}, ({t}){R(i.rb)}) | ctx->xer_so;')

    def i_cmpi(self, fn, a, i, o):
        t = 'int64_t' if i.l10 else 'int32_t'
        o.append(f'ctx->cr[{i.crfd}] = cmp_s(({t}){R(i.ra)}, {i.simm}) | ctx->xer_so;')

    def i_cmpli(self, fn, a, i, o):
        t = 'uint64_t' if i.l10 else 'uint32_t'
        o.append(f'ctx->cr[{i.crfd}] = cmp_u(({t}){R(i.ra)}, {i.uimm}u) | ctx->xer_so;')

    # -- XO arithmetic (with optional Rc) -------------------------------------
    def _xo(self, i, o, expr, dst=None):
        d = dst if dst is not None else i.rd
        o.append(f'{R(d)} = {expr};')
        if i.rc:
            o.append(f'CR0({R(d)});')

    def i_add(self, fn, a, i, o): self._xo(i, o, f'{R(i.ra)} + {R(i.rb)}')
    def i_subf(self, fn, a, i, o): self._xo(i, o, f'{R(i.rb)} - {R(i.ra)}')
    def i_neg(self, fn, a, i, o): self._xo(i, o, f'(uint64_t)-(int64_t){R(i.ra)}')
    def i_mullw(self, fn, a, i, o): self._xo(i, o, f'(uint64_t)((int64_t)(int32_t){R(i.ra)} * (int64_t)(int32_t){R(i.rb)})')
    def i_mulld(self, fn, a, i, o): self._xo(i, o, f'{R(i.ra)} * {R(i.rb)}')
    def i_mulhw(self, fn, a, i, o): self._xo(i, o, f'(uint64_t)(int64_t)(int32_t)(((int64_t)(int32_t){R(i.ra)} * (int64_t)(int32_t){R(i.rb)}) >> 32)')
    def i_mulhwu(self, fn, a, i, o): self._xo(i, o, f'(uint32_t)(((uint64_t)(uint32_t){R(i.ra)} * (uint32_t){R(i.rb)}) >> 32)')
    def i_mulhd(self, fn, a, i, o): self._xo(i, o, f'mulhd_({R(i.ra)}, {R(i.rb)})')
    def i_mulhdu(self, fn, a, i, o): self._xo(i, o, f'mulhdu_({R(i.ra)}, {R(i.rb)})')
    def i_divw(self, fn, a, i, o): self._xo(i, o, f'divw_({R(i.ra)}, {R(i.rb)})')
    def i_divwu(self, fn, a, i, o): self._xo(i, o, f'divwu_({R(i.ra)}, {R(i.rb)})')
    def i_divd(self, fn, a, i, o): self._xo(i, o, f'divd_({R(i.ra)}, {R(i.rb)})')
    def i_divdu(self, fn, a, i, o): self._xo(i, o, f'divdu_({R(i.ra)}, {R(i.rb)})')

    def _carry(self, i, o, x, y, cin):
        o.append(f'{R(i.rd)} = adde64({x}, {y}, {cin}, &ctx->xer_ca);')
        if i.rc:
            o.append(f'CR0({R(i.rd)});')

    def i_addc(self, fn, a, i, o): self._carry(i, o, R(i.ra), R(i.rb), '0')
    def i_adde(self, fn, a, i, o): self._carry(i, o, R(i.ra), R(i.rb), 'ctx->xer_ca')
    def i_addze(self, fn, a, i, o): self._carry(i, o, R(i.ra), '0', 'ctx->xer_ca')
    def i_addme(self, fn, a, i, o): self._carry(i, o, R(i.ra), '~0ull', 'ctx->xer_ca')
    def i_subfc(self, fn, a, i, o): self._carry(i, o, f'~{R(i.ra)}', R(i.rb), '1')
    def i_subfe(self, fn, a, i, o): self._carry(i, o, f'~{R(i.ra)}', R(i.rb), 'ctx->xer_ca')
    def i_subfze(self, fn, a, i, o): self._carry(i, o, f'~{R(i.ra)}', '0', 'ctx->xer_ca')
    def i_subfme(self, fn, a, i, o): self._carry(i, o, f'~{R(i.ra)}', '~0ull', 'ctx->xer_ca')

    def i_addic(self, fn, a, i, o):
        o.append(f'{R(i.rd)} = adde64({R(i.ra)}, {hx(i.simm & (2**64 - 1))}, 0, &ctx->xer_ca);')

    def i_addic_(self, fn, a, i, o):
        self.i_addic(fn, a, i, o)
        o.append(f'CR0({R(i.rd)});')

    def i_subfic(self, fn, a, i, o):
        o.append(f'{R(i.rd)} = adde64(~{R(i.ra)}, {hx(i.simm & (2**64 - 1))}, 1, &ctx->xer_ca);')

    # -- logical / shifts with Rc ------------------------------------------
    def _x(self, i, o, expr):
        o.append(f'{R(i.ra)} = {expr};')
        if i.rc:
            o.append(f'CR0({R(i.ra)});')

    def i_and(self, fn, a, i, o): self._x(i, o, f'{R(i.rs)} & {R(i.rb)}')
    def i_or(self, fn, a, i, o): self._x(i, o, f'{R(i.rs)} | {R(i.rb)}')
    def i_xor(self, fn, a, i, o): self._x(i, o, f'{R(i.rs)} ^ {R(i.rb)}')
    def i_nand(self, fn, a, i, o): self._x(i, o, f'~({R(i.rs)} & {R(i.rb)})')
    def i_nor(self, fn, a, i, o): self._x(i, o, f'~({R(i.rs)} | {R(i.rb)})')
    def i_eqv(self, fn, a, i, o): self._x(i, o, f'~({R(i.rs)} ^ {R(i.rb)})')
    def i_andc(self, fn, a, i, o): self._x(i, o, f'{R(i.rs)} & ~{R(i.rb)}')
    def i_orc(self, fn, a, i, o): self._x(i, o, f'{R(i.rs)} | ~{R(i.rb)}')
    def i_extsb(self, fn, a, i, o): self._x(i, o, f'(uint64_t)(int64_t)(int8_t){R(i.rs)}')
    def i_extsh(self, fn, a, i, o): self._x(i, o, f'(uint64_t)(int64_t)(int16_t){R(i.rs)}')
    def i_extsw(self, fn, a, i, o): self._x(i, o, f'(uint64_t)(int64_t)(int32_t){R(i.rs)}')
    def i_cntlzw(self, fn, a, i, o): self._x(i, o, f'cntlzw_({R(i.rs)})')
    def i_cntlzd(self, fn, a, i, o): self._x(i, o, f'cntlzd_({R(i.rs)})')
    def i_slw(self, fn, a, i, o): self._x(i, o, f'({R(i.rb)} & 0x20) ? 0 : (uint32_t)((uint32_t){R(i.rs)} << ({R(i.rb)} & 31))')
    def i_srw(self, fn, a, i, o): self._x(i, o, f'({R(i.rb)} & 0x20) ? 0 : ((uint32_t){R(i.rs)} >> ({R(i.rb)} & 31))')
    def i_sld(self, fn, a, i, o): self._x(i, o, f'({R(i.rb)} & 0x40) ? 0 : ({R(i.rs)} << ({R(i.rb)} & 63))')
    def i_srd(self, fn, a, i, o): self._x(i, o, f'({R(i.rb)} & 0x40) ? 0 : ({R(i.rs)} >> ({R(i.rb)} & 63))')
    def i_sraw(self, fn, a, i, o): self._x(i, o, f'sraw_(ctx, {R(i.rs)}, {R(i.rb)})')
    def i_srad(self, fn, a, i, o): self._x(i, o, f'srad_(ctx, {R(i.rs)}, {R(i.rb)})')
    def i_srawi(self, fn, a, i, o): self._x(i, o, f'sraw_(ctx, {R(i.rs)}, {i.sh32})')
    def i_sradi(self, fn, a, i, o): self._x(i, o, f'srad_(ctx, {R(i.rs)}, {i.sh64})')

    def i_andi_(self, fn, a, i, o):
        o.append(f'{R(i.ra)} = {R(i.rs)} & {i.uimm}u; CR0({R(i.ra)});')

    def i_andis_(self, fn, a, i, o):
        o.append(f'{R(i.ra)} = {R(i.rs)} & 0x{i.uimm << 16:x}u; CR0({R(i.ra)});')

    # -- rotates ------------------------------------------------------------
    def i_rlwinm(self, fn, a, i, o):
        mb, me, sh = i.mb32, i.me32, i.sh32
        if mb <= me:
            m = mask64(mb + 32, me + 32)
            self._x(i, o, f'(uint64_t)(rotl32((uint32_t){R(i.rs)}, {sh}) & 0x{m:x}u)')
        else:
            m = mask64(mb + 32, me + 32)
            self._x(i, o, f'(rotl64(dup32({R(i.rs)}), {sh}) & {hx(m)})')

    def i_rlwnm(self, fn, a, i, o):
        m = mask64(i.mb32 + 32, i.me32 + 32)
        self._x(i, o, f'(rotl64(dup32({R(i.rs)}), {R(i.rb)} & 31) & {hx(m)})')

    def i_rlwimi(self, fn, a, i, o):
        m = mask64(i.mb32 + 32, i.me32 + 32)
        self._x(i, o, f'(rotl64(dup32({R(i.rs)}), {i.sh32}) & {hx(m)}) | ({R(i.ra)} & {hx(~m & (2**64 - 1))})')

    def i_rldicl(self, fn, a, i, o):
        self._x(i, o, f'(rotl64({R(i.rs)}, {i.sh64}) & {hx(mask64(i.mb64, 63))})')

    def i_rldicr(self, fn, a, i, o):
        self._x(i, o, f'(rotl64({R(i.rs)}, {i.sh64}) & {hx(mask64(0, i.mb64))})')

    def i_rldic(self, fn, a, i, o):
        self._x(i, o, f'(rotl64({R(i.rs)}, {i.sh64}) & {hx(mask64(i.mb64, 63 - i.sh64))})')

    def i_rldimi(self, fn, a, i, o):
        m = mask64(i.mb64, 63 - i.sh64)
        self._x(i, o, f'(rotl64({R(i.rs)}, {i.sh64}) & {hx(m)}) | ({R(i.ra)} & {hx(~m & (2**64 - 1))})')

    def i_rldcl(self, fn, a, i, o):
        self._x(i, o, f'(rotl64({R(i.rs)}, {R(i.rb)} & 63) & {hx(mask64(i.mb64, 63))})')

    def i_rldcr(self, fn, a, i, o):
        self._x(i, o, f'(rotl64({R(i.rs)}, {R(i.rb)} & 63) & {hx(mask64(0, i.mb64))})')

    # -- multi-word / string ---------------------------------------------------
    def i_lmw(self, fn, a, i, o):
        o.append(f'{{ uint64_t ea = {ra0(i)} + {s16(i.simm)}; for (int k = {i.rd}; k < 32; k++, ea += 4) ctx->r[k] = ld32(ea); }}')

    def i_stmw(self, fn, a, i, o):
        o.append(f'{{ uint64_t ea = {ra0(i)} + {s16(i.simm)}; for (int k = {i.rs}; k < 32; k++, ea += 4) st32(ea, (uint32_t)ctx->r[k]); }}')

    def i_lswi(self, fn, a, i, o): o.append(f'ppu_lswi(ctx, {i.rd}, (uint32_t){ra0(i)}, {i.rb or 32});')
    def i_stswi(self, fn, a, i, o): o.append(f'ppu_stswi(ctx, {i.rs}, (uint32_t){ra0(i)}, {i.rb or 32});')
    def i_lswx(self, fn, a, i, o): o.append(f'ppu_lswi(ctx, {i.rd}, (uint32_t)({ra0(i)} + {R(i.rb)}), ctx->xer_cnt);')
    def i_stswx(self, fn, a, i, o): o.append(f'ppu_stswi(ctx, {i.rs}, (uint32_t)({ra0(i)} + {R(i.rb)}), ctx->xer_cnt);')

    # -- atomics -----------------------------------------------------------
    def i_lwarx(self, fn, a, i, o): o.append(f'{R(i.rd)} = lwarx_(ctx, {ra0(i)} + {R(i.rb)});')
    def i_ldarx(self, fn, a, i, o): o.append(f'{R(i.rd)} = ldarx_(ctx, {ra0(i)} + {R(i.rb)});')

    def i_stwcx_(self, fn, a, i, o):
        o.append(f'ctx->cr[0] = (stwcx_(ctx, {ra0(i)} + {R(i.rb)}, {R(i.rs)}) ? 2 : 0) | ctx->xer_so;')

    def i_stdcx_(self, fn, a, i, o):
        o.append(f'ctx->cr[0] = (stdcx_(ctx, {ra0(i)} + {R(i.rb)}, {R(i.rs)}) ? 2 : 0) | ctx->xer_so;')

    # -- FPU with Rc / compares -------------------------------------------------
    def i_fcmpu(self, fn, a, i, o): o.append(f'ctx->cr[{i.crfd}] = cmp_f({F(i.fra)}, {F(i.frb)});')
    i_fcmpo = i_fcmpu

    def i_mffs(self, fn, a, i, o): o.append(f'{F(i.frd)} = u2d(ctx->fpscr);')
    def i_mtfsf(self, fn, a, i, o): o.append(f'ctx->fpscr = (uint32_t)d2u({F(i.frb)});')
    def i_mtfsfi(self, fn, a, i, o): o.append(';')
    def i_mtfsb0(self, fn, a, i, o): o.append(f'ctx->fpscr &= ~(0x80000000u >> {i.rd});')
    def i_mtfsb1(self, fn, a, i, o): o.append(f'ctx->fpscr |= (0x80000000u >> {i.rd});')
    def i_mcrfs(self, fn, a, i, o): o.append(f'ctx->cr[{i.crfd}] = (ctx->fpscr >> (28 - 4 * {i.crfs})) & 15;')

    # -- VMX with immediates / CR6 ---------------------------------------------
    def i_vsldoi(self, fn, a, i, o): o.append(f'{V(i.vd)} = vsldoi({V(i.va)}, {V(i.vb)}, {i.vshb});')
    def i_mfvscr(self, fn, a, i, o): o.append(f'{V(i.vd)} = vspltisw(0); {V(i.vd)}.u32[0] = ctx->vscr;')
    def i_mtvscr(self, fn, a, i, o): o.append(f'ctx->vscr = {V(i.vb)}.u32[0];')


# Simple one-liners: mnemonic -> lambda(Insn) -> C statement
def _ld(fn, upd=False, idx=False):
    def g(i):
        ea = (f'{R(i.ra)} + {R(i.rb)}' if upd else f'{ra0(i)} + {R(i.rb)}') if idx else \
             (f'{R(i.ra)} + {s16(i.simm)}' if upd else f'{ra0(i)} + {s16(i.simm)}')
        s = f'{{ uint64_t ea = {ea}; {fn(i)}; '
        if upd:
            s += f'{R(i.ra)} = ea; '
        return s + '}'
    return g


def _ds(fn, upd=False):
    def g(i):
        ea = f'{R(i.ra)} + {s16(i.ds)}' if upd else f'{ra0(i)} + {s16(i.ds)}'
        return f'{{ uint64_t ea = {ea}; {fn(i)}; ' + (f'{R(i.ra)} = ea; ' if upd else '') + '}'
    return g


SIMPLE = {
    'addi': lambda i: f'{R(i.rd)} = {ra0(i)} + {s16(i.simm)};',
    'addis': lambda i: f'{R(i.rd)} = {ra0(i)} + (int64_t){i.simm * 65536};',
    'mulli': lambda i: f'{R(i.rd)} = {R(i.ra)} * (uint64_t){s16(i.simm)};',
    'ori': lambda i: ';' if i.rs == i.ra and i.uimm == 0 else f'{R(i.ra)} = {R(i.rs)} | {i.uimm}u;',
    'oris': lambda i: f'{R(i.ra)} = {R(i.rs)} | 0x{i.uimm << 16:x}u;',
    'xori': lambda i: f'{R(i.ra)} = {R(i.rs)} ^ {i.uimm}u;',
    'xoris': lambda i: f'{R(i.ra)} = {R(i.rs)} ^ 0x{i.uimm << 16:x}u;',
    # barriers / cache hints
    'sync': lambda i: '__atomic_thread_fence(__ATOMIC_SEQ_CST);',
    'isync': lambda i: ';', 'eieio': lambda i: '__atomic_thread_fence(__ATOMIC_SEQ_CST);',
    'dcbt': lambda i: ';', 'dcbtst': lambda i: ';', 'dcbf': lambda i: ';', 'dcbst': lambda i: ';',
    'icbi': lambda i: ';', 'dst': lambda i: ';', 'dstst': lambda i: ';', 'dss': lambda i: ';',
    'dcbz': lambda i: f'memset(g_mem + (EA({ra0(i)} + {R(i.rb)}) & ~127u), 0, 128);',
}

_INT_LOADS = {
    'lbz': 'ld8(ea)', 'lhz': 'ld16(ea)', 'lha': '(uint64_t)(int64_t)(int16_t)ld16(ea)', 'lwz': 'ld32(ea)',
}
for nm, ex in _INT_LOADS.items():
    f = (lambda ex: lambda i: f'{R(i.rd)} = {ex}')(ex)
    SIMPLE[nm] = _ld(f)
    SIMPLE[nm + 'u'] = _ld(f, upd=True)
    SIMPLE[nm + 'x'] = _ld(f, idx=True)
    SIMPLE[nm + 'ux'] = _ld(f, upd=True, idx=True)
SIMPLE['lwax'] = _ld(lambda i: f'{R(i.rd)} = (uint64_t)(int64_t)(int32_t)ld32(ea)', idx=True)
SIMPLE['lwaux'] = _ld(lambda i: f'{R(i.rd)} = (uint64_t)(int64_t)(int32_t)ld32(ea)', upd=True, idx=True)
SIMPLE['ldx'] = _ld(lambda i: f'{R(i.rd)} = ld64(ea)', idx=True)
SIMPLE['ldux'] = _ld(lambda i: f'{R(i.rd)} = ld64(ea)', upd=True, idx=True)
SIMPLE['ld'] = _ds(lambda i: f'{R(i.rd)} = ld64(ea)')
SIMPLE['ldu'] = _ds(lambda i: f'{R(i.rd)} = ld64(ea)', upd=True)
SIMPLE['lwa'] = _ds(lambda i: f'{R(i.rd)} = (uint64_t)(int64_t)(int32_t)ld32(ea)')
SIMPLE['lwbrx'] = _ld(lambda i: f'{R(i.rd)} = __builtin_bswap32(ld32(ea))', idx=True)
SIMPLE['lhbrx'] = _ld(lambda i: f'{R(i.rd)} = __builtin_bswap16(ld16(ea))', idx=True)

_INT_STORES = {'stb': 'st8(ea, (uint8_t){s})', 'sth': 'st16(ea, (uint16_t){s})', 'stw': 'st32(ea, (uint32_t){s})'}
for nm, ex in _INT_STORES.items():
    f = (lambda ex: lambda i: ex.format(s=R(i.rs)))(ex)
    SIMPLE[nm] = _ld(f)
    SIMPLE[nm + 'u'] = _ld(f, upd=True)
    SIMPLE[nm + 'x'] = _ld(f, idx=True)
    SIMPLE[nm + 'ux'] = _ld(f, upd=True, idx=True)
SIMPLE['std'] = _ds(lambda i: f'st64(ea, {R(i.rs)})')
SIMPLE['stdu'] = _ds(lambda i: f'st64(ea, {R(i.rs)})', upd=True)
SIMPLE['stdx'] = _ld(lambda i: f'st64(ea, {R(i.rs)})', idx=True)
SIMPLE['stdux'] = _ld(lambda i: f'st64(ea, {R(i.rs)})', upd=True, idx=True)
SIMPLE['stwbrx'] = _ld(lambda i: f'st32(ea, __builtin_bswap32((uint32_t){R(i.rs)}))', idx=True)
SIMPLE['sthbrx'] = _ld(lambda i: f'st16(ea, __builtin_bswap16((uint16_t){R(i.rs)}))', idx=True)

_FP_MEM = {
    'lfs': lambda i: f'{F(i.frd)} = (double)u2f(ld32(ea))',
    'lfd': lambda i: f'{F(i.frd)} = u2d(ld64(ea))',
    'stfs': lambda i: f'st32(ea, f2u((float){F(i.frs)}))',
    'stfd': lambda i: f'st64(ea, d2u({F(i.frs)}))',
}
for nm, f in _FP_MEM.items():
    SIMPLE[nm] = _ld(f)
    SIMPLE[nm + 'u'] = _ld(f, upd=True)
    SIMPLE[nm + 'x'] = _ld(f, idx=True)
    SIMPLE[nm + 'ux'] = _ld(f, upd=True, idx=True)
SIMPLE['stfiwx'] = _ld(lambda i: f'st32(ea, (uint32_t)d2u({F(i.frs)}))', idx=True)

# FPU arithmetic. Rc forms (CR1) are not modelled.
_FA = {
    'fadd': '{a} + {b}', 'fsub': '{a} - {b}', 'fmul': '{a} * {c}', 'fdiv': '{a} / {b}',
    'fmadd': 'fma({a}, {c}, {b})', 'fmsub': 'fma({a}, {c}, -{b})',
    'fnmadd': '-fma({a}, {c}, {b})', 'fnmsub': '-fma({a}, {c}, -{b})',
    'fsqrt': 'sqrt({b})', 'fsel': 'fsel_({a}, {c}, {b})', 'frsqrte': '1.0 / sqrt({b})',
}
for nm, ex in _FA.items():
    SIMPLE[nm] = (lambda ex: lambda i: f'{F(i.frd)} = ' + ex.format(a=F(i.fra), b=F(i.frb), c=F(i.frc)) + ';')(ex)
    if nm not in ('fsel', 'frsqrte'):
        SIMPLE[nm + 's'] = (lambda ex: lambda i: f'{F(i.frd)} = rnd_s(' + ex.format(a=F(i.fra), b=F(i.frb), c=F(i.frc)) + ');')(ex)
SIMPLE['fres'] = lambda i: f'{F(i.frd)} = rnd_s(1.0 / {F(i.frb)});'
SIMPLE.update({
    'frsp': lambda i: f'{F(i.frd)} = rnd_s({F(i.frb)});',
    'fmr': lambda i: f'{F(i.frd)} = {F(i.frb)};',
    'fneg': lambda i: f'{F(i.frd)} = -{F(i.frb)};',
    'fabs': lambda i: f'{F(i.frd)} = fabs({F(i.frb)});',
    'fnabs': lambda i: f'{F(i.frd)} = -fabs({F(i.frb)});',
    'fctiw': lambda i: f'{F(i.frd)} = u2d(fctiw_({F(i.frb)}));',
    'fctiwz': lambda i: f'{F(i.frd)} = u2d(fctiwz_({F(i.frb)}));',
    'fctid': lambda i: f'{F(i.frd)} = u2d(fctid_({F(i.frb)}));',
    'fctidz': lambda i: f'{F(i.frd)} = u2d(fctidz_({F(i.frb)}));',
    'fcfid': lambda i: f'{F(i.frd)} = (double)(int64_t)d2u({F(i.frb)});',
})

# VMX memory
_VM = {
    'lvx': lambda i: f'{V(i.vd)} = ldv(ea)', 'lvxl': lambda i: f'{V(i.vd)} = ldv(ea)',
    'lvebx': lambda i: f'{V(i.vd)} = ldv(ea)', 'lvehx': lambda i: f'{V(i.vd)} = ldv(ea)',
    'lvewx': lambda i: f'{V(i.vd)} = ldv(ea)',
    'stvx': lambda i: f'stv(ea, {V(i.vs)})', 'stvxl': lambda i: f'stv(ea, {V(i.vs)})',
    'lvsl': lambda i: f'{V(i.vd)} = lvsl_(ea)', 'lvsr': lambda i: f'{V(i.vd)} = lvsr_(ea)',
    'lvlx': lambda i: f'{V(i.vd)} = lvlx_(ea)', 'lvlxl': lambda i: f'{V(i.vd)} = lvlx_(ea)',
    'lvrx': lambda i: f'{V(i.vd)} = lvrx_(ea)', 'lvrxl': lambda i: f'{V(i.vd)} = lvrx_(ea)',
    'stvlx': lambda i: f'stvlx_(ea, {V(i.vs)})', 'stvlxl': lambda i: f'stvlx_(ea, {V(i.vs)})',
    'stvrx': lambda i: f'stvrx_(ea, {V(i.vs)})', 'stvrxl': lambda i: f'stvrx_(ea, {V(i.vs)})',
    'stvebx': lambda i: f'stvebx_(ea, {V(i.vs)})', 'stvehx': lambda i: f'stvehx_(ea, {V(i.vs)})',
    'stvewx': lambda i: f'stvewx_(ea, {V(i.vs)})',
}
for nm, f in _VM.items():
    SIMPLE[nm] = _ld(f, idx=True)

from . import ppc as _ppc  # noqa: E402

_V_UNARY = {'vrefp', 'vrsqrtefp', 'vexptefp', 'vlogefp', 'vrfin', 'vrfiz', 'vrfip', 'vrfim',
            'vupkhsb', 'vupklsb', 'vupkhsh', 'vupklsh', 'vupkhpx', 'vupklpx'}
_V_UIMM_B = {'vspltb', 'vsplth', 'vspltw', 'vcfux', 'vcfsx', 'vctuxs', 'vctsxs'}
_V_SIMM = {'vspltisb', 'vspltish', 'vspltisw'}
for nm in _ppc.VX.values():
    if nm in ('mfvscr', 'mtvscr'):
        continue
    if nm in _V_UNARY:
        SIMPLE[nm] = (lambda nm: lambda i: f'{V(i.vd)} = {nm}({V(i.vb)});')(nm)
    elif nm in _V_UIMM_B:
        SIMPLE[nm] = (lambda nm: lambda i: f'{V(i.vd)} = {nm}({V(i.vb)}, {i.vuimm});')(nm)
    elif nm in _V_SIMM:
        SIMPLE[nm] = (lambda nm: lambda i: f'{V(i.vd)} = {nm}({i.vsimm});')(nm)
    else:
        SIMPLE[nm] = (lambda nm: lambda i: f'{V(i.vd)} = {nm}({V(i.va)}, {V(i.vb)});')(nm)
for nm in _ppc.VA.values():
    if nm != 'vsldoi':
        SIMPLE[nm] = (lambda nm: lambda i: f'{V(i.vd)} = {nm}({V(i.va)}, {V(i.vb)}, {V(i.vc)});')(nm)
for nm in _ppc.VC.values():
    SIMPLE[nm] = (lambda nm: lambda i: f'{V(i.vd)} = {nm}({V(i.va)}, {V(i.vb)});'
                  + (f' ctx->cr[6] = vcr6({V(i.vd)});' if i.vrc else ''))(nm)
for nm in ('vcmpbfp',):
    SIMPLE[nm] = lambda i: (f'{V(i.vd)} = vcmpbfp({V(i.va)}, {V(i.vb)});'
                            + (f' ctx->cr[6] = (({V(i.vd)}.u64[0] | {V(i.vd)}.u64[1]) == 0) ? 2 : 0;' if i.vrc else ''))
