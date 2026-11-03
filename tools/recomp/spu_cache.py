"""Build step: compile SPU code captured at runtime (spu_cache/) to C++.

    python -m recomp.spu_cache --cache <build/spu_cache> --out <build/generated>

Runs on every build; files are rewritten only when their content changes.
"""
import argparse
import os

from . import elf, spu


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cache', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--elf')   # the EBOOT: its modules get run-time entry points too
    a = ap.parse_args()
    log = lambda *m: print('[recomp]', *m, flush=True)
    spu.generate_cache(a.cache, a.out, log)
    extra = spu.read_extra_entries(a.cache)
    if a.elf and os.path.isfile(a.elf) and any(not k.startswith('rt_') for k in extra):
        spu.generate(elf.load(a.elf), a.out, log, extra)


if __name__ == '__main__':
    main()
