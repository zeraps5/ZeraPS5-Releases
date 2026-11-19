# SPDX-License-Identifier: GPL-2.0-or-later
"""Resolves host stacks in a log: the GPU thread's at a stuck suspend point, a host
thread's at a crash ("(<function> at <address>)" gives the executable's base).

Usage: python -I tools/host_stack.py <ps5c.exe> <log>
Needs nm and addr2line (MSYS2 ucrt64) on PATH, and the same build that wrote the log.
"""
import re
import subprocess
import sys


def main():
    exe, log = sys.argv[1], sys.argv[2]
    nm = subprocess.check_output(["nm", exe], text=True)
    symbols = {l.split()[-1]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
    base = static = None
    for line in open(log, encoding="utf-8", errors="replace"):
        m = re.search(r"\((\w+) at ([0-9A-Fa-fx]+)\)", line)
        if m and m.group(1) in symbols:
            base, static = int(m.group(2), 16), symbols[m.group(1)]
            print(line.strip())
            continue
        m = re.search(r"host stack:(.*)", line)
        if not m or base is None:
            continue
        addrs = [hex(int(a, 16) - base + static) for a in m.group(1).split()]
        out = subprocess.check_output(["addr2line", "-f", "-C", "-e", exe] + addrs, text=True).splitlines()
        for i in range(0, len(out) - 1, 2):
            print(f"    {out[i]}  {out[i + 1]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
