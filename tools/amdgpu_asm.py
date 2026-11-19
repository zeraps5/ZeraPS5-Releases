#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Assembles PS5 GPU (gfx1013, RDNA2) instructions with clang's AMDGPU assembler.

Used as the reference for the shader decoder: generating opcode tables and
checking disassembly round trips. Needs a clang built with the AMDGPU target
(MSYS2's is). Library use: assemble(lines) -> list of dword lists, one per
line (None where the line does not assemble).
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

TARGET = os.environ.get("PS5C_GPU_TARGET", "gfx1013")  # PS5 GPU: GFX10.1 ISA (keeps v_mac/v_mad)
CLANG = os.environ.get("PS5C_CLANG") or shutil.which("clang") or r"C:\msys64\ucrt64\bin\clang.exe"


def _text_section(obj):
    shoff = struct.unpack_from("<Q", obj, 0x28)[0]
    shnum, shstrndx = struct.unpack_from("<HH", obj, 0x3C)
    secs = [struct.unpack_from("<IIQQQQIIQQ", obj, shoff + i * 64) for i in range(shnum)]
    strtab_off = secs[shstrndx][4]
    for s in secs:
        name = obj[strtab_off + s[0]:].split(b"\0", 1)[0]
        if name == b".text":
            return obj[s[4]:s[4] + s[5]]
    return b""


def _assemble_block(lines, tmp):
    src = os.path.join(tmp, "in.s")
    out = os.path.join(tmp, "out.o")
    # A label per line gives each instruction's offset through the symbol table;
    # simpler: separate each line by a marker instruction of known encoding.
    marker = "s_nop 0x3ff"  # 0xbf8003ff, never used by real code
    with open(src, "w") as f:
        for line in lines:
            f.write(marker + "\n" + line + "\n")
        f.write(marker + "\n")
    r = subprocess.run([CLANG, "-cc1as", "-triple", "amdgcn-amd-amdhsa", "-target-cpu", TARGET,
                        "-filetype", "obj", "-o", out, src], capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stderr
    text = _text_section(open(out, "rb").read())
    words = list(struct.unpack("<%dI" % (len(text) // 4), text))
    result, cur = [], None
    for w in words:
        if w == 0xBF8003FF:
            if cur is not None:
                result.append(cur)
            cur = []
        else:
            cur.append(w)
    return result, ""


_ERR = re.compile(r"in\.s:(\d+):\d+: error")


def assemble(lines):
    """One dword list per input line, None for lines clang rejects."""
    out = [None] * len(lines)
    with tempfile.TemporaryDirectory() as tmp:
        idx = list(range(len(lines)))
        # clang reports every bad line: drop them and assemble again.
        for _ in range(64):
            if not idx:
                break
            res, err = _assemble_block([lines[i] for i in idx], tmp)
            if res is not None and len(res) == len(idx):
                for i, words in zip(idx, res):
                    out[i] = words
                break
            # Source line 2k+2 (1-based) holds input k (markers in between).
            bad = {(int(m.group(1)) - 2) // 2 for m in _ERR.finditer(err)}
            if not bad:
                break
            idx = [i for k, i in enumerate(idx) if k not in bad]
    return out


if __name__ == "__main__":
    lines = [l.strip() for l in sys.stdin if l.strip()]
    for line, words in zip(lines, assemble(lines)):
        print("%-60s %s" % (line, " ".join("%08x" % w for w in words) if words is not None else "ERROR"))
