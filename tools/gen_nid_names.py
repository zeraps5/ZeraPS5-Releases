#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds data/nid_names.txt (one symbol name per line) from shadPS4's
src/core/aerolib/aerolib.inl (GPL-2.0-or-later).

Every entry is verified: the NID is recomputed from the name and entries whose
NID doesn't match are dropped, so the output only contains names we can prove.

usage: gen_nid_names.py <path/to/aerolib.inl> <output.txt> [source description]
"""
import base64
import hashlib
import re
import struct
import sys

SALT = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")
# Long entries are wrapped over several lines, so match across the whole file.
STUB_RE = re.compile(r'STUB\(\s*"([A-Za-z0-9+\-]{11})",\s*([^)\s]+)\s*\)')


def nid_of(name: str) -> str:
    digest = hashlib.sha1(name.encode() + SALT).digest()
    value = struct.unpack("<Q", digest[:8])[0]
    return base64.b64encode(struct.pack(">Q", value), b"+-").decode()[:11]


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, out = sys.argv[1], sys.argv[2]
    origin = sys.argv[3] if len(sys.argv) > 3 else src

    names, total, rejected = set(), 0, 0
    with open(src, encoding="utf-8", errors="replace") as f:
        text = f.read()
    for m in STUB_RE.finditer(text):
        total += 1
        nid, name = m.groups()
        if nid_of(name) == nid:
            names.add(name)
        else:
            rejected += 1

    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# PS5C NID name database: one symbol name per line; NIDs are computed from the names.\n")
        f.write(f"# Derived from shadPS4 aerolib.inl (GPL-2.0-or-later): {origin}\n")
        f.write(f"# {len(names)} verified names ({rejected} of {total} entries rejected: NID mismatch)\n")
        for name in sorted(names):
            f.write(name + "\n")

    print(f"{len(names)} names written to {out} ({rejected}/{total} rejected)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
