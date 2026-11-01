"""Build step: compile SPU code captured at runtime (spu_cache/) to C++.

    python -m recomp.spu_cache --cache <build/spu_cache> --out <build/generated>

Runs on every build; files are rewritten only when their content changes.
"""
import argparse

from . import spu


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cache', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    spu.generate_cache(a.cache, a.out, lambda *m: print('[recomp]', *m, flush=True))


if __name__ == '__main__':
    main()
