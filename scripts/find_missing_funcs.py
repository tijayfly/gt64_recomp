#!/usr/bin/env python3
"""List code addresses the game takes the address of (lui/addiu pairs, including `sym + 0xN`, and data words) that
are not function starts in the splat disassembly but follow a `jr $ra` (so they start a function rather than being a
switch case). Splat merges such small functions (callbacks reached only by pointer) into the previous function, and
the runtime then fails with "Failed to find function at 0x...". Prints symbol_addrs.txt lines.

Usage: scripts/find_missing_funcs.py   (run from the repo root after a splat split)
"""
import re
import struct

ROM = open("rom/GT64 - Championship Edition (USA).z64", "rb").read()
ASM = [open(p).read() for p in ("split/asm/1050.s", "split/asm/C7920.s")]
TEXT = [(0x80000400, 0x8005C9D0, 0x80000400 - 0x1000), (0x801A2520, 0x801C03A0, 0x801A2520 - 0xC7920)]
DATA = [(0x5D800, 0xC7920), (0xE57A0, 0xF0500)]


def rom_of(a):
    for lo, hi, base in TEXT:
        if lo <= a < hi:
            return a - base
    return None


def word(a):
    o = rom_of(a)
    return struct.unpack(">I", ROM[o:o + 4])[0]


def main():
    src = "\n".join(ASM)
    starts = set(int(x, 16) for x in re.findall(r"glabel \w+_([0-9A-F]{8})\b", src))
    symaddr = {m.group(1): int(m.group(2), 16) for m in re.finditer(r"glabel ((?:func|D)_([0-9A-F]{8}))", src)}
    # functions with register jumps (switch statements): addresses inside them are likely case labels
    jtfuncs = []
    for m in re.finditer(r"glabel \w+_([0-9A-F]{8})(.*?)endlabel", src, re.S):
        if re.search(r"jr\s+\$(?!ra)", m.group(2)):
            addrs = [int(x, 16) for x in re.findall(r"/\* [0-9A-F]+ ([0-9A-F]{8})", m.group(2))]
            jtfuncs.append((int(m.group(1), 16), max(addrs)))
    cand = set()
    for m in re.finditer(r"%lo\(((?:func|D)_([0-9A-F]{8}))(?: \+ 0x([0-9A-F]+))?\)", src):
        a = int(m.group(2), 16) + (int(m.group(3), 16) if m.group(3) else 0)
        cand.add(a)
    for m in re.finditer(r"alabel D_([0-9A-F]{8})", src):
        cand.add(int(m.group(1), 16))
    for lo, hi in DATA:
        for o in range(lo, hi, 4):
            cand.add(struct.unpack(">I", ROM[o:o + 4])[0])
    out = []
    for a in sorted(cand):
        if a % 4 or a in starts or rom_of(a) is None:
            continue
        if any(lo <= a <= hi for lo, hi in jtfuncs):
            continue
        p = a - 8
        while rom_of(p) is not None and word(p) == 0 and word(p + 4) == 0:
            p -= 4
        if rom_of(p) is not None and word(p) == 0x03E00008:
            out.append(a)
    for a in out:
        print(f"func_{a:08X} = 0x{a:08X}; // type:func")


if __name__ == "__main__":
    main()
