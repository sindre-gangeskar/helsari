"""Readable SPU disassembly from the generated code, for reverse-engineering jobs.

    from spudis import load, fmt, SKIP
    ops, read = load(37, 0xd65480)   # module number, job address in the EBOOT
    for pc, op, word in ops: print(hex(pc), fmt(pc, op, word))
"""
import re,struct,sys,collections
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from recomp import elf
E=elf.load(os.path.join(ROOT, 'build/generated/decrypt/EBOOT.elf'))
def load(m,src,base=0x4000):
    s=open(os.path.join(ROOT, f'build/generated/spu/spu_m{m}.cpp'),errors='replace').read()
    ops=[(int(pc,16),o,int(x,16)) for o,x,pc in re.findall(r'step_r<(\w+)>\(t, g, 0x([0-9a-f]+)u, 0x([0-9a-f]+)u\)',s)]
    return ops,lambda ls,n:E.read(src+(ls-base),n)
def fmt(p,o,x):
    rt=x&0x7f;ra=(x>>7)&0x7f;rb=(x>>14)&0x7f;i10=(x>>14)&0x3ff;i7=rb;i16=(x>>7)&0xffff
    i10=i10-1024 if i10>=512 else i10; i7s=i7-128 if i7>=64 else i7; s16=i16-65536 if i16>=32768 else i16
    if o in('FMA','FMS','FNMS','SHUFB','SELB','MPYA'): return f'r{(x>>21)&0x7f} = {o}(r{ra},r{rb},r{rt})'
    if o in('LQD',): return f'r{rt} = [{i10*16}+r{ra}]'
    if o=='STQD': return f'[{i10*16}+r{ra}] = r{rt}'
    if o=='LQX': return f'r{rt} = [r{ra}+r{rb}]'
    if o=='STQX': return f'[r{ra}+r{rb}] = r{rt}'
    if o=='LQR': return f'r{rt} = [{p+s16*4:#x}]'
    if o=='LQA': return f'r{rt} = [{(i16<<2):#x}]'
    if o=='STQR': return f'[{p+s16*4:#x}] = r{rt}'
    if o in('AI','ANDI','ORI','CEQI','CGTI','SFI','CLGTI','XORI','MPYI','AHI','MPYUI'): return f'r{rt} = {o}(r{ra},{i10})'
    if o in('IL',): return f'r{rt} = {s16}'
    if o in('ILHU',): return f'r{rt} = {i16:#x}<<16'
    if o in('IOHL',): return f'r{rt} |= {i16:#x}'
    if o=='ILA': return f'r{rt} = {(x>>7)&0x3ffff:#x}'
    if o in('BR',): return f'goto {p+s16*4:#x}'
    if o in('BRSL',): return f'call {p+s16*4:#x} (r{rt})'
    if o in('BRASL','BRA'): return f'{o} {(i16<<2):#x}'
    if o in('BRZ','BRNZ','BRHZ','BRHNZ'): return f'{o} r{rt} -> {p+s16*4:#x}'
    if o in('BI','BISL','BIZ','BINZ'): return f'{o} r{ra}'
    if o.endswith('I') or o in('ROTQBYI',): return f'r{rt} = {o}(r{ra},{i7s})'
    if o in('RDCH','WRCH'): return f'{o} ch{ra} r{rt}'
    return f'r{rt} = {o}(r{ra},r{rb})'
SKIP=('NOP','LNOP','HBRR','HBR','HBRA','SYNC','DSYNC')
