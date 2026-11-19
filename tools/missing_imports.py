#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Merge the per-game lists written by PS5C_LIST_UNIMPLEMENTED=<file> (one
"library<TAB>name" line per import without HLE) into one table, most used first.

With --protos <dir>..., each function's C/C++ prototype is looked up in other
emulators' sources, to flag the ones that write through a pointer argument
(an output the game reads back, so a stub returning 0 leaves garbage there).

    ps5c run <eboot> with PS5C_LIST_UNIMPLEMENTED=lists/NN.txt   (one per game)
    python tools/missing_imports.py lists/*.txt --protos <src dirs> -o merged.tsv

Output columns: games, library, function, outputs, prototype, list files."""
import argparse
import collections
import os
import re
import sys

PROTO = re.compile(r'\b\w+\s*\**\s*(?:\w+_ABI\s+|APS5_VABI\s+)?(sce\w+|_\w+)\s*\(([^)]*)\)\s*\{', re.S)


def outputs(args):
    if args is None:
        return '?'
    outs = [a.strip() for a in args.split(',')
            if '*' in a and 'const ' not in a.split('*')[0]]
    return '; '.join(outs) if outs else '-'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('lists', nargs='+')
    ap.add_argument('--protos', nargs='*', default=[], help='source trees to search for prototypes')
    ap.add_argument('-o', '--output', default='-')
    a = ap.parse_args()

    use = collections.defaultdict(set)
    lib_of = {}
    for path in a.lists:
        tag = os.path.splitext(os.path.basename(path))[0]
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                parts = line.rstrip('\r\n').split('\t')
                if len(parts) == 2:
                    use[parts[1]].add(tag)
                    lib_of[parts[1]] = parts[0]

    protos = {}
    for root in a.protos:
        for dp, _, files in os.walk(root):
            for name in files:
                if not name.endswith(('.c', '.cpp', '.h', '.hpp')):
                    continue
                try:
                    with open(os.path.join(dp, name), encoding='utf-8', errors='replace') as f:
                        text = f.read()
                except OSError:
                    continue
                for m in PROTO.finditer(text):
                    if m.group(1) in use and m.group(1) not in protos:
                        protos[m.group(1)] = ' '.join(m.group(2).split())

    rows = sorted(((len(g), lib_of[fn], fn, outputs(protos.get(fn)), protos.get(fn, ''), ','.join(sorted(g)))
                   for fn, g in use.items()), key=lambda r: (-r[0], r[1], r[2]))
    out = sys.stdout if a.output == '-' else open(a.output, 'w', encoding='utf-8')
    for r in rows:
        out.write('%d\t%s\t%s\t%s\t%s\t%s\n' % r)
    if out is not sys.stdout:
        out.close()
    with_out = sum(1 for r in rows if r[3] not in ('?', '-'))
    print('%d lists, %d distinct functions, %d with a prototype, %d with outputs' %
          (len(a.lists), len(rows), sum(1 for r in rows if r[3] != '?'), with_out), file=sys.stderr)


if __name__ == '__main__':
    main()
