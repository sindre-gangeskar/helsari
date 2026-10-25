"""PS3 function NIDs.

A NID is the first 4 bytes (little-endian) of SHA-1(name + fixed salt).
We never ship a name database: names are recovered at build time by
hashing candidate strings (the runtime's own HLE names, plus any strings
found in a user-supplied binary such as rpcs3.exe) and matching them.
"""
import hashlib
import re
import struct

_SALT = bytes.fromhex('6759659904250490566427499489741A')


def nid(name: str) -> int:
    return struct.unpack('<I', hashlib.sha1(name.encode() + _SALT).digest()[:4])[0]


_IDENT = re.compile(rb'[A-Za-z_][A-Za-z0-9_]{3,63}')


def names_from_binary(path):
    """Every identifier-looking ASCII string in a file."""
    data = open(path, 'rb').read()
    return {m.group().decode() for m in _IDENT.finditer(data)}


def resolve(wanted_nids, candidates):
    want = set(wanted_nids)
    out = {}
    for n in candidates:
        h = nid(n)
        if h in want:
            out[h] = n
    return out
