#!/usr/bin/env python3
"""Generate an N64Recomp symbols TOML from splat's asm output.

Reads `nonmatching <name>, <size>` lines and the `/* ROM VRAM WORD */` comments that follow each function to recover
rom/vram/size for every function, for each code section of GT64.
"""
import re
import sys
from pathlib import Path

# name, rom start, vram start, rom end of CPU code, asm files
SECTIONS = [
    # main: entrypoint + game + libultra. rspboot/aspMain RSP code follows at 0x5D5D0.
    (".text", 0x1000, 0x80000400, 0x5D5D0, ["split/asm/1000.s", "split/asm/1050.s"]),
    # Front-end overlay, DMA'd from ROM 0xC7920 to 0x801A2520 (the race loads graphics data into the same RAM).
    (".ovl_front", 0xC7920, 0x801A2520, 0xE57A0, ["split/asm/C7920.s"]),
]

FUNC_RE = re.compile(r"^nonmatching (\w+), 0x([0-9A-Fa-f]+)")
INSN_RE = re.compile(r"/\* ([0-9A-Fa-f]+) ([0-9A-Fa-f]{8}) [0-9A-Fa-f]{8} \*/")


def parse_funcs(asm_paths, renames):
    funcs = []
    pending = None
    for path in asm_paths:
        for line in Path(path).read_text().splitlines():
            m = FUNC_RE.match(line)
            if m:
                pending = (m.group(1), int(m.group(2), 16))
                continue
            if pending:
                m = INSN_RE.search(line)
                if m:
                    name, size = pending
                    vram = int(m.group(2), 16)
                    funcs.append((renames.get(vram, name), vram, size))
                    pending = None
    return sorted(funcs, key=lambda f: f[1])


def load_renames(paths):
    """Load `name = 0xADDR;` files; later files override earlier ones."""
    renames = {}
    for path in filter(None, paths.split(",")):
        for line in Path(path).read_text().splitlines():
            line = line.split("//")[0].strip()
            m = re.match(r"(\w+)\s*=\s*0x([0-9A-Fa-f]+)", line)
            if m:
                renames[int(m.group(2), 16)] = m.group(1)
    return renames


def main():
    out_path, renames_path = sys.argv[1:3]
    renames = load_renames(renames_path)
    out = []
    total = 0
    for name, rom, vram, rom_end, asm in SECTIONS:
        end_vram = vram + rom_end - rom
        funcs = [f for f in parse_funcs(asm, renames) if vram <= f[1] < end_vram]
        out += [
            "[[section]]",
            f'name = "{name}"',
            f"rom = 0x{rom:08X}",
            f"vram = 0x{vram:08X}",
            f"size = 0x{rom_end - rom:X}",
            "",
            "functions = [",
        ]
        for fname, fvram, size in funcs:
            out.append(f'    {{ name = "{fname}", vram = 0x{fvram:08X}, size = 0x{size:X} }},')
        out += ["]", ""]
        total += len(funcs)
    Path(out_path).write_text("\n".join(out) + "\n")
    print(f"wrote {total} functions to {out_path}")


if __name__ == "__main__":
    main()
