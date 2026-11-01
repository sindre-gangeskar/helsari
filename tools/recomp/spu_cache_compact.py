"""Merge duplicate captures in spu_cache/ (the same program captured with different data).

    python -m recomp.spu_cache_compact --cache <build/spu_cache>

Two captures are the same program when they share LS base and size and agree on
every quadword either of them executed; they merge into one file with the union
of both masks. The runtime applies the same rule when saving (spu_capture.cpp).
"""
import argparse
import os
import struct

from .spu import read_cache_files, write_capture


def same_code(a, b):
    """a, b: (mask, image). Equal on every quadword executed in either."""
    for q, (x, y) in enumerate(zip(a[0], b[0])):
        if (x or y) and a[1][16 * q:16 * q + 16] != b[1][16 * q:16 * q + 16]:
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cache', required=True)
    a = ap.parse_args()
    groups = {}                                   # (ls_base, size) -> [[mask, image, [files]]]
    n = 0
    for path, ls_base, mask, image in read_cache_files(a.cache):
        n += 1
        merged = groups.setdefault((ls_base, len(image)), [])
        for g in merged:
            if same_code((g[0], g[1]), (mask, image)):
                img = bytearray(g[1])
                for q, x in enumerate(mask):
                    if x and not g[0][q]:
                        g[0][q] = True
                        img[16 * q:16 * q + 16] = image[16 * q:16 * q + 16]
                g[1] = bytes(img)
                g[2].append(path)
                break
        else:
            merged.append([list(mask), image, [path]])
    kept = 0
    for (ls_base, _), merged in groups.items():
        for mask, image, files in merged:
            for f in files:
                os.remove(f)
            write_capture(a.cache, ls_base, mask, image)
            kept += 1
    print(f'[recomp] spu_cache: {n} captures merged into {kept} programs')


if __name__ == '__main__':
    main()
