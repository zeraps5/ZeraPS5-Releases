#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Round-trip check of the RDNA2 disassembler.

Disassembles shader binaries with `ps5c disasm`, assembles the text with
clang (tools/amdgpu_asm.py) and compares the result with the original words.

    python tools/check_disasm.py [ps5c.exe] shader.bin...
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from amdgpu_asm import assemble  # noqa: E402


def write_corpus(path):
    """One instance of every opcode the table generator confirmed (gfx1013)."""
    import struct
    import amdgpu_asm
    import gen_rdna2_ops as gen
    amdgpu_asm.TARGET = "gfx1013"
    gen.probe_all()
    with open(path, "wb") as f:
        for _, words in gen.CORPUS:
            f.write(struct.pack("<%dI" % len(words), *words))
    return len(gen.CORPUS)


def main():
    args = sys.argv[1:]
    if args and args[0] == "--corpus":
        path = args[1] if len(args) > 1 else "rdna2_corpus.bin"
        print("corpus: %d instructions -> %s" % (write_corpus(path), path))
        args = [path]
    exe = args.pop(0) if args and args[0].endswith(".exe") else os.path.join("build", "gcc", "ps5c.exe")
    total = exact = skipped = 0
    mismatches = []
    for path in args:
        out = subprocess.run([exe, "disasm", path], capture_output=True, text=True).stdout
        items = []
        for line in out.splitlines():
            flag, rest = line[0], line[1:]
            _, rest = rest.split(":", 1)
            hexpart, text = rest[:37].split(), rest[37:].strip()
            words = [int(h, 16) for h in hexpart]
            total += 1
            if flag == "!":
                skipped += 1
                continue
            items.append((words, text))
        for (words, text), got in zip(items, assemble([t for _, t in items])):
            if got == words:
                exact += 1
            else:
                mismatches.append((path, text, words, got))
    for path, text, words, got in mismatches[:60]:
        print("MISMATCH %s: %-50s want %s got %s" % (os.path.basename(path), text, " ".join("%08x" % w for w in words),
                                                   " ".join("%08x" % w for w in got) if got else "ERROR"))
    print("%d instructions: %d exact, %d mismatched, %d not printable" % (total, exact, len(mismatches), skipped))
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
