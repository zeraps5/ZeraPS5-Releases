#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Names for IL2CPP code addresses.

Reads global-metadata.dat (version 29) and a relocated dump of the module
that holds the generated code (PS5C_DUMP_MODULE=Il2CppUserAssemblies writes
user/<name>_<base>.bin), finds each assembly's Il2CppCodeGenModule and maps
method pointers to "Namespace.Type::Method".

  python tools/il2cpp_symbols.py <global-metadata.dat> <dump.bin> [rva ...]

Without RVAs, writes <dump>.symbols.txt (sorted "rva name" lines). With RVAs
(hex, relative to the module base), prints the method containing each one.
"""
import bisect
import os
import re
import struct
import sys


def cstr(buf, off):
    end = buf.index(b"\0", off)
    return buf[off:end].decode("utf-8", "replace")


class Metadata:
    def __init__(self, path):
        d = open(path, "rb").read()
        magic, version = struct.unpack_from("<Ii", d, 0)
        if magic != 0xFAB11BAF or version != 29:
            raise SystemExit("unsupported metadata (magic %x, version %d)" % (magic, version))
        h = struct.unpack_from("<64i", d, 8)
        pair = lambda i: (h[2 * i], h[2 * i + 1])
        self.d = d
        self.str_off = pair(2)[0]
        m_off, m_size = pair(5)
        t_off, t_size = pair(19)
        i_off, i_size = pair(20)
        self.methods = [struct.unpack_from("<iiiiiIHHHH", d, m_off + k * 32) for k in range(m_size // 32)]
        self.types = []
        for k in range(t_size // 88):
            ints = struct.unpack_from("<8i8i", d, t_off + k * 88)
            counts = struct.unpack_from("<8H", d, t_off + k * 88 + 64)
            self.types.append((ints, counts))
        self.images = [struct.unpack_from("<10i", d, i_off + k * 40) for k in range(i_size // 40)]

    def s(self, idx):
        return cstr(self.d, self.str_off + idx)

    def type_name(self, t):
        ints, _ = self.types[t]
        name, ns, declaring = self.s(ints[0]), self.s(ints[1]), ints[3]
        if declaring >= 0 and declaring < len(self.types):
            pass  # declaringTypeIndex is a type index (Il2CppType), not a definition: keep the short form
        return (ns + "." if ns else "") + name


def find_modules(md, img, base):
    """moduleName pointer -> (count, methodPointers) for each image."""
    out = {}
    for image in md.images:
        name = md.s(image[0])
        needle = name.encode() + b"\0"
        pos = img.find(b"\0" + needle)
        while pos >= 0:
            addr = base + pos + 1
            for m in re.finditer(re.escape(struct.pack("<Q", addr)), img):
                at = m.start()
                if at % 8:
                    continue
                count = struct.unpack_from("<I", img, at + 8)[0]
                ptrs = struct.unpack_from("<Q", img, at + 16)[0]
                if 0 < count < 1000000 and base <= ptrs < base + len(img):
                    out[name] = (count, ptrs - base)
                    break
            if name in out:
                break
            pos = img.find(b"\0" + needle, pos + 1)
    return out


def build(md, img, base):
    mods = find_modules(md, img, base)
    syms = []
    for image in md.images:
        name = md.s(image[0])
        if name not in mods:
            continue
        count, table = mods[name]
        type_start, type_count = image[2], image[3]
        for t in range(type_start, type_start + type_count):
            ints, counts = md.types[t]
            method_start, method_count = ints[9], counts[0]
            tname = md.type_name(t)
            for k in range(method_start, method_start + method_count):
                m = md.methods[k]
                rid = m[5] & 0xFFFFFF
                if rid == 0 or rid > count:
                    continue
                ptr = struct.unpack_from("<Q", img, table + (rid - 1) * 8)[0]
                if ptr and base <= ptr < base + len(img):
                    syms.append((ptr - base, "%s::%s" % (tname, md.s(m[0]))))
    syms.sort()
    return syms, mods


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    md = Metadata(sys.argv[1])
    dump = sys.argv[2]
    base = int(re.search(r"_([0-9a-fA-F]+)\.bin$", dump).group(1), 16)
    img = open(dump, "rb").read()
    syms, mods = build(md, img, base)
    print("%d assemblies with code, %d methods" % (len(mods), len(syms)), file=sys.stderr)
    if len(sys.argv) == 3:
        with open(dump + ".symbols.txt", "w") as f:
            for rva, name in syms:
                f.write("%08x %s\n" % (rva, name))
        print("wrote " + dump + ".symbols.txt", file=sys.stderr)
        return 0
    keys = [r for r, _ in syms]
    for a in sys.argv[3:]:
        rva = int(a, 16)
        i = bisect.bisect_right(keys, rva) - 1
        print("%08x  %s+0x%x" % (rva, syms[i][1], rva - syms[i][0]) if i >= 0 else "%08x  ?" % rva)
    return 0


if __name__ == "__main__":
    sys.exit(main())
