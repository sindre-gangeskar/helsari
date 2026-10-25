"""Minimal PS3 (CellOS Lv-2) ELF64 big-endian parser.

Only reads what the recompiler needs: loadable segments, the process
parameters, the TLS image, the .opd function-descriptor table and the
PRX import stubs (which Sony library functions the game calls).
"""
import struct
from dataclasses import dataclass, field

PT_LOAD = 1
PT_TLS = 7
PT_PROC_PARAM = 0x60000001
PT_PROC_PRX = 0x60000002


@dataclass
class Segment:
    type: int
    flags: int
    offset: int
    vaddr: int
    filesz: int
    memsz: int


@dataclass
class Section:
    name: str
    type: int
    addr: int
    offset: int
    size: int


@dataclass
class Import:
    module: str
    nid: int
    stub_addr: int  # address of the 32-bit slot the loader fills with an OPD pointer


@dataclass
class Elf:
    data: bytes
    entry: int
    segments: list = field(default_factory=list)
    sections: list = field(default_factory=list)

    # -- memory access by virtual address ---------------------------------
    def seg_for(self, va):
        for s in self.segments:
            if s.type == PT_LOAD and s.vaddr <= va < s.vaddr + s.filesz:
                return s
        return None

    def read(self, va, n):
        s = self.seg_for(va)
        if s is None:
            raise ValueError(f"address {va:#x} not in file image")
        o = s.offset + va - s.vaddr
        return self.data[o:o + n]

    def u32(self, va):
        return struct.unpack('>I', self.read(va, 4))[0]

    def cstr(self, va):
        s = self.seg_for(va)
        o = s.offset + va - s.vaddr
        return self.data[o:self.data.index(b'\0', o)].decode('latin-1')

    def section(self, name):
        for s in self.sections:
            if s.name == name:
                return s
        return None

    # -- PS3 specifics ------------------------------------------------------
    def seg(self, type_):
        return next((s for s in self.segments if s.type == type_), None)

    def process_param(self):
        s = self.seg(PT_PROC_PARAM)
        size, magic, ver, sdk, prio, stack, pagesize, ppc_seg = struct.unpack(
            '>8I', self.data[s.offset:s.offset + 32])
        return dict(sdk_version=sdk, primary_prio=prio, primary_stacksize=stack,
                    malloc_pagesize=pagesize, ppc_seg=ppc_seg)

    def tls(self):
        s = self.seg(PT_TLS)
        return (s.vaddr, s.filesz, s.memsz) if s else (0, 0, 0)

    def text_range(self):
        """The executable PT_LOAD segment (flags bit 0 = X)."""
        s = next(s for s in self.segments if s.type == PT_LOAD and s.flags & 1)
        return s.vaddr, s.vaddr + s.filesz

    def opd(self):
        """Function descriptors: list of (func_addr, toc)."""
        # Section names are stripped in retail binaries; .opd is the section
        # holding the entry point's descriptor.
        sec = self.section('.opd') or next(
            s for s in self.sections if s.addr <= self.entry < s.addr + s.size)
        out = []
        for va in range(sec.addr, sec.addr + sec.size, 8):
            out.append(struct.unpack('>II', self.read(va, 8)))
        return out

    def imports(self):
        s = self.seg(PT_PROC_PRX)
        (size, magic, ver, sdk, ent_s, ent_e, stub_s, stub_e) = struct.unpack(
            '>8I', self.data[s.offset:s.offset + 32])
        out = []
        va = stub_s
        while va < stub_e:
            hdr = self.read(va, 0x2c)
            ssize = hdr[0]
            num_func, = struct.unpack('>H', hdr[6:8])
            name_p, fnid_p, fstub_p = struct.unpack('>III', hdr[0x10:0x1c])
            mod = self.cstr(name_p)
            for i in range(num_func):
                out.append(Import(mod, self.u32(fnid_p + 4 * i), fstub_p + 4 * i))
            va += ssize
        return out


def load(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF':
        raise ValueError(f"{path}: not a decrypted ELF (did RPCS3 --decrypt run?)")
    (e_type, e_mach, e_ver, e_entry, e_phoff, e_shoff, e_flags, e_ehs,
     e_phes, e_phn, e_shes, e_shn, e_shstr) = struct.unpack('>HHIQQQIHHHHHH', d[16:64])
    elf = Elf(d, e_entry)
    for i in range(e_phn):
        o = e_phoff + i * e_phes
        t, f, off, va, pa, fs, ms, al = struct.unpack('>IIQQQQQQ', d[o:o + 56])
        elf.segments.append(Segment(t, f, off, va, fs, ms))
    raw = []
    for i in range(e_shn):
        o = e_shoff + i * e_shes
        raw.append(struct.unpack('>IIQQQQIIQQ', d[o:o + 64]))
    if raw:
        stro = raw[e_shstr][4]
        for n, t, fl, a, off, sz, *_ in raw:
            name = d[stro + n:d.index(b'\0', stro + n)].decode()
            elf.sections.append(Section(name, t, a, off, sz))
    return elf
