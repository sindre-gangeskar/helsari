"""PowerPC (Cell PPU) instruction decoder: integer, branch, FPU and VMX.

decode(word) -> (mnemonic, Insn) or (None, Insn) for unknown encodings.
Field naming follows the IBM Cell BE PPU manual.
"""


class Insn:
    __slots__ = ('w',)

    def __init__(self, w):
        self.w = w

    def bits(self, start, end):  # IBM bit numbering, 0 = MSB, inclusive
        return (self.w >> (31 - end)) & ((1 << (end - start + 1)) - 1)

    # common fields
    opcd = property(lambda s: s.bits(0, 5))
    rd = rs = rt = to = vd = vs = frd = frs = property(lambda s: s.bits(6, 10))
    ra = va = fra = property(lambda s: s.bits(11, 15))
    rb = vb = frb = property(lambda s: s.bits(16, 20))
    frc = vc = property(lambda s: s.bits(21, 25))
    rc = property(lambda s: s.w & 1)
    lk = property(lambda s: s.w & 1)
    aa = property(lambda s: (s.w >> 1) & 1)
    oe = property(lambda s: (s.w >> 10) & 1)
    bo = property(lambda s: s.bits(6, 10))
    bi = property(lambda s: s.bits(11, 15))
    crfd = property(lambda s: s.bits(6, 8))
    crfs = property(lambda s: s.bits(11, 13))
    l10 = property(lambda s: s.bits(10, 10))
    uimm = property(lambda s: s.w & 0xffff)
    simm = property(lambda s: (s.w & 0xffff) - ((s.w & 0x8000) << 1))
    ds = property(lambda s: ((s.w & 0xfffc) - ((s.w & 0x8000) << 1)))

    @property
    def bd(self):
        v = self.w & 0xfffc
        return v - 0x10000 if v & 0x8000 else v

    @property
    def li(self):
        v = self.w & 0x03fffffc
        return v - 0x04000000 if v & 0x02000000 else v

    # rotate fields (32-bit forms)
    sh32 = property(lambda s: s.bits(16, 20))
    mb32 = property(lambda s: s.bits(21, 25))
    me32 = property(lambda s: s.bits(26, 30))

    # 64-bit rotate fields
    @property
    def sh64(self):
        return self.bits(16, 20) | (self.bits(30, 30) << 5)

    @property
    def mb64(self):  # also me64; 6-bit field stored rotated
        x = self.bits(21, 26)
        return ((x & 1) << 5) | (x >> 1)

    spr = property(lambda s: (s.bits(16, 20) << 5) | s.bits(11, 15))
    crm = property(lambda s: s.bits(12, 19))
    # VMX
    vuimm = property(lambda s: s.bits(11, 15))
    vsimm = property(lambda s: s.bits(11, 15) - (32 if s.bits(11, 11) else 0))
    vshb = property(lambda s: s.bits(22, 25))
    vrc = property(lambda s: s.bits(21, 21))
    fm = property(lambda s: s.bits(7, 14))


def _t(*pairs):
    return dict(pairs)


OP_D = {
    2: 'tdi', 3: 'twi', 7: 'mulli', 8: 'subfic', 10: 'cmpli', 11: 'cmpi',
    12: 'addic', 13: 'addic.', 14: 'addi', 15: 'addis', 16: 'bc', 17: 'sc',
    18: 'b', 20: 'rlwimi', 21: 'rlwinm', 23: 'rlwnm', 24: 'ori', 25: 'oris',
    26: 'xori', 27: 'xoris', 28: 'andi.', 29: 'andis.',
    32: 'lwz', 33: 'lwzu', 34: 'lbz', 35: 'lbzu', 36: 'stw', 37: 'stwu',
    38: 'stb', 39: 'stbu', 40: 'lhz', 41: 'lhzu', 42: 'lha', 43: 'lhau',
    44: 'sth', 45: 'sthu', 46: 'lmw', 47: 'stmw', 48: 'lfs', 49: 'lfsu',
    50: 'lfd', 51: 'lfdu', 52: 'stfs', 53: 'stfsu', 54: 'stfd', 55: 'stfdu',
}

OP19 = {0: 'mcrf', 16: 'bclr', 33: 'crnor', 129: 'crandc', 150: 'isync',
        193: 'crxor', 225: 'crnand', 257: 'crand', 289: 'creqv', 417: 'crorc',
        449: 'cror', 528: 'bcctr'}

OP31 = {
    0: 'cmp', 4: 'tw', 6: 'lvsl', 7: 'lvebx', 19: 'mfcr', 20: 'lwarx',
    21: 'ldx', 23: 'lwzx', 24: 'slw', 26: 'cntlzw', 27: 'sld', 28: 'and',
    32: 'cmpl', 38: 'lvsr', 39: 'lvehx', 53: 'ldux', 54: 'dcbst', 55: 'lwzux',
    58: 'cntlzd', 60: 'andc', 68: 'td', 71: 'lvewx', 83: 'mfmsr', 84: 'ldarx',
    86: 'dcbf', 87: 'lbzx', 103: 'lvx', 119: 'lbzux', 124: 'nor', 135: 'stvebx',
    144: 'mtcrf', 149: 'stdx', 150: 'stwcx.', 151: 'stwx', 167: 'stvehx',
    181: 'stdux', 183: 'stwux', 199: 'stvewx', 214: 'stdcx.', 215: 'stbx',
    231: 'stvx', 246: 'dcbtst', 247: 'stbux', 278: 'dcbt', 279: 'lhzx',
    284: 'eqv', 311: 'lhzux', 316: 'xor', 339: 'mfspr', 341: 'lwax',
    342: 'dst', 343: 'lhax', 359: 'lvxl', 371: 'mftb', 373: 'lwaux',
    374: 'dstst', 375: 'lhaux', 407: 'sthx', 412: 'orc', 439: 'sthux',
    444: 'or', 467: 'mtspr', 476: 'nand', 487: 'stvxl', 519: 'lvlx',
    533: 'lswx', 534: 'lwbrx', 535: 'lfsx', 536: 'srw', 539: 'srd', 551: 'lvrx',
    567: 'lfsux', 597: 'lswi', 598: 'sync', 599: 'lfdx', 631: 'lfdux', 647: 'stvlx',
    661: 'stswx', 662: 'stwbrx', 663: 'stfsx', 679: 'stvrx', 695: 'stfsux', 725: 'stswi', 727: 'stfdx',
    759: 'stfdux', 775: 'lvlxl', 790: 'lhbrx', 792: 'sraw', 794: 'srad',
    807: 'lvrxl', 822: 'dss', 824: 'srawi', 854: 'eieio', 903: 'stvlxl',
    918: 'sthbrx', 922: 'extsh', 935: 'stvrxl', 954: 'extsb', 982: 'icbi',
    983: 'stfiwx', 986: 'extsw', 1014: 'dcbz',
}
OP31_XO = {  # 9-bit XO-form (OE bit separate)
    8: 'subfc', 9: 'mulhdu', 10: 'addc', 11: 'mulhwu', 40: 'subf', 73: 'mulhd',
    75: 'mulhw', 104: 'neg', 136: 'subfe', 138: 'adde', 200: 'subfze',
    202: 'addze', 232: 'subfme', 233: 'mulld', 234: 'addme', 235: 'mullw',
    266: 'add', 457: 'divdu', 459: 'divwu', 489: 'divd', 491: 'divw',
}

OP59 = {18: 'fdivs', 20: 'fsubs', 21: 'fadds', 22: 'fsqrts', 24: 'fres',
        25: 'fmuls', 28: 'fmsubs', 29: 'fmadds', 30: 'fnmsubs', 31: 'fnmadds'}
OP63_A = {18: 'fdiv', 20: 'fsub', 21: 'fadd', 22: 'fsqrt', 23: 'fsel',
          25: 'fmul', 26: 'frsqrte', 28: 'fmsub', 29: 'fmadd', 30: 'fnmsub',
          31: 'fnmadd'}
OP63_X = {0: 'fcmpu', 12: 'frsp', 14: 'fctiw', 15: 'fctiwz', 32: 'fcmpo',
          38: 'mtfsb1', 40: 'fneg', 64: 'mcrfs', 70: 'mtfsb0', 72: 'fmr',
          134: 'mtfsfi', 136: 'fnabs', 264: 'fabs', 583: 'mffs', 711: 'mtfsf',
          814: 'fctid', 815: 'fctidz', 846: 'fcfid'}

VA = {32: 'vmhaddshs', 33: 'vmhraddshs', 34: 'vmladduhm', 36: 'vmsumubm',
      37: 'vmsummbm', 38: 'vmsumuhm', 39: 'vmsumuhs', 40: 'vmsumshm',
      41: 'vmsumshs', 42: 'vsel', 43: 'vperm', 44: 'vsldoi', 46: 'vmaddfp',
      47: 'vnmsubfp'}
VC = {6: 'vcmpequb', 70: 'vcmpequh', 134: 'vcmpequw', 198: 'vcmpeqfp',
      454: 'vcmpgefp', 518: 'vcmpgtub', 582: 'vcmpgtuh', 646: 'vcmpgtuw',
      710: 'vcmpgtfp', 774: 'vcmpgtsb', 838: 'vcmpgtsh', 902: 'vcmpgtsw',
      966: 'vcmpbfp'}
VX = {
    0: 'vaddubm', 64: 'vadduhm', 128: 'vadduwm', 384: 'vaddcuw', 512: 'vaddubs',
    576: 'vadduhs', 640: 'vadduws', 768: 'vaddsbs', 832: 'vaddshs', 896: 'vaddsws',
    1024: 'vsububm', 1088: 'vsubuhm', 1152: 'vsubuwm', 1408: 'vsubcuw',
    1536: 'vsububs', 1600: 'vsubuhs', 1664: 'vsubuws', 1792: 'vsubsbs',
    1856: 'vsubshs', 1920: 'vsubsws', 2: 'vmaxub', 66: 'vmaxuh', 130: 'vmaxuw',
    258: 'vmaxsb', 322: 'vmaxsh', 386: 'vmaxsw', 514: 'vminub', 578: 'vminuh',
    642: 'vminuw', 770: 'vminsb', 834: 'vminsh', 898: 'vminsw', 1026: 'vavgub',
    1090: 'vavguh', 1154: 'vavguw', 1282: 'vavgsb', 1346: 'vavgsh',
    1410: 'vavgsw', 4: 'vrlb', 68: 'vrlh', 132: 'vrlw', 260: 'vslb', 324: 'vslh',
    388: 'vslw', 452: 'vsl', 516: 'vsrb', 580: 'vsrh', 644: 'vsrw', 708: 'vsr',
    772: 'vsrab', 836: 'vsrah', 900: 'vsraw', 1028: 'vand', 1092: 'vandc',
    1156: 'vor', 1220: 'vxor', 1284: 'vnor', 1540: 'mfvscr', 1604: 'mtvscr',
    8: 'vmuloub', 72: 'vmulouh', 264: 'vmulosb', 328: 'vmulosh', 520: 'vmuleub',
    584: 'vmuleuh', 776: 'vmulesb', 840: 'vmulesh', 1544: 'vsum4ubs',
    1800: 'vsum4sbs', 1608: 'vsum4shs', 1672: 'vsum2sws', 1928: 'vsumsws',
    10: 'vaddfp', 74: 'vsubfp', 266: 'vrefp', 330: 'vrsqrtefp', 394: 'vexptefp',
    458: 'vlogefp', 522: 'vrfin', 586: 'vrfiz', 650: 'vrfip', 714: 'vrfim',
    778: 'vcfux', 842: 'vcfsx', 906: 'vctuxs', 970: 'vctsxs', 1034: 'vmaxfp',
    1098: 'vminfp', 12: 'vmrghb', 76: 'vmrghh', 140: 'vmrghw', 268: 'vmrglb',
    332: 'vmrglh', 396: 'vmrglw', 524: 'vspltb', 588: 'vsplth', 652: 'vspltw',
    780: 'vspltisb', 844: 'vspltish', 908: 'vspltisw', 1036: 'vslo', 1100: 'vsro',
    14: 'vpkuhum', 78: 'vpkuwum', 142: 'vpkuhus', 206: 'vpkuwus', 270: 'vpkshus',
    334: 'vpkswus', 398: 'vpkshss', 462: 'vpkswss', 526: 'vupkhsb', 590: 'vupkhsh',
    654: 'vupklsb', 718: 'vupklsh', 782: 'vpkpx', 846: 'vupkhpx', 974: 'vupklpx',
}


def decode(w):
    i = Insn(w)
    op = w >> 26
    m = OP_D.get(op)
    if m:
        return m, i
    if op == 4:
        m = VA.get(w & 0x3f) or VX.get(w & 0x7ff) or VC.get(w & 0x3ff)
        return m, i
    if op == 19:
        return OP19.get((w >> 1) & 0x3ff), i
    if op == 30:
        x = (w >> 2) & 7
        if x < 4:
            return ('rldicl', 'rldicr', 'rldic', 'rldimi')[x], i
        x = (w >> 1) & 0xf
        return {8: 'rldcl', 9: 'rldcr'}.get(x), i
    if op == 31:
        xo = (w >> 1) & 0x3ff
        if (xo >> 1) == 413:
            return 'sradi', i
        return OP31.get(xo) or OP31_XO.get(xo & 0x1ff), i
    if op == 58:
        return {0: 'ld', 1: 'ldu', 2: 'lwa'}.get(w & 3), i
    if op == 62:
        return {0: 'std', 1: 'stdu'}.get(w & 3), i
    if op == 59:
        return OP59.get((w >> 1) & 0x1f), i
    if op == 63:
        a = OP63_A.get((w >> 1) & 0x1f)
        if a:
            return a, i
        return OP63_X.get((w >> 1) & 0x3ff), i
    return None, i
