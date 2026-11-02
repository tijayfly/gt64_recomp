<div align="center">

# GT 64 Recompiled

**GT 64: Championship Edition (N64, 1998), as a native PC program.**
Gamepad support · any resolution · widescreen · 60 FPS (planned)

</div>

---

> [!WARNING]
> **Work in progress: it doesn't boot yet.** The ROM is mapped and the recompiler configs are written; the first
> build is next. See [Status](#status).

This is a static recompilation of *GT 64: Championship Edition* (N64, USA) made with
[N64Recomp](https://github.com/N64Recomp/N64Recomp). The game's MIPS code is translated to C, compiled for your PC
and run on [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime), with [RT64](https://github.com/rt64/rt64)
for rendering. It is not an emulator: the game runs as a native program. The app is built like
[Mission: Impossible Recompiled](https://github.com/nickallevato/mi_n64_exe) and
[BanjoRecomp](https://github.com/BanjoRecomp/BanjoRecomp), on [RecompFrontend](https://github.com/N64Recomp/RecompFrontend).

> **You need your own ROM.** No ROM or game data is in this repository. Use a dump of
> *GT64 - Championship Edition (USA)* in big-endian `.z64` format, sha1 `b941246af78748f4e081f496dac0a9e2056d3d8d`.
> A byte-swapped `.n64`/`.v64` dump has to be converted to `.z64` first.

## Status

- [x] ROM mapped: main code, one front-end code overlay, RSP microcodes
- [x] libultra functions named (156), so the runtime can replace them
- [x] N64Recomp and RSPRecomp configs
- [ ] First build boots to the title screen
- [ ] Menus, races, saves, audio
- [ ] Gamepad defaults for racing (triggers for throttle/brake)
- [ ] 60 FPS
- [ ] Windows x64 build

## Building

Linux, with clang. The ROM goes in `rom/`, which git ignores.

```sh
git clone --recurse-submodules https://github.com/tijayfly/gt64_recomp.git && cd gt64_recomp
mkdir -p rom && cp "<path>/GT64 - Championship Edition (USA).z64" rom/
sudo apt-get install cmake ninja-build clang lld libsdl2-dev libgtk-3-dev libfreetype-dev
```

**1. Disassemble and recompile the game** (`tools/` is ignored):

```sh
python3 -m venv tools/venv && tools/venv/bin/pip install 'splat64[mips]'

cd split && ln -sf "../rom/GT64 - Championship Edition (USA).z64" baserom.z64 \
  && ../tools/venv/bin/python -m splat split gt64.yaml && cd ..

cmake -S lib/N64ModernRuntime/N64Recomp -B build/n64recomp -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/n64recomp --target N64Recomp RSPRecomp
scripts/recomp.sh        # game code -> RecompiledFuncs/, audio microcode -> rsp/
```

**2. Build and run the app:**

```sh
cmake -S . -B build/app -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/app
GT_ROM="rom/GT64 - Championship Edition (USA).z64" build/app/GT64Recompiled
```

For a Windows build cross-compiled from Linux, see [docs/WINDOWS_CROSS.md](docs/WINDOWS_CROSS.md).

## How it works

<details>
<summary>ROM layout</summary>

There's no decomp of this game, so the function list comes from a [splat](https://github.com/ethteck/splat)
disassembly instead of an ELF.

| What | ROM | VRAM |
|---|---|---|
| entrypoint | `0x1000` | `0x80000400` |
| main `.text` (game + libultra, 1329 funcs) | `0x1050`–`0x5D5D0` | `0x80000450`– |
| rspboot, then n_aspMain text (`0xE20`) | `0x5D5D0`, `0x5D6A0` | `0x8005C9D0`, `0x8005CAA0` |
| data / rodata (incl. n_aspMain data at `0xC3C70`, F3DEX 1.23 family data) | `0x5D800`–`0xC7920` | |
| bss (`0xDB800`) | — | `0x800C6D20` |
| front-end overlay `.text` (667 funcs), then its data | `0xC7920`–`0xE57A0`–`0xF0500` | `0x801A2520` |
| assets (race data is also loaded at `0x801A2520`) | `0xF0500`–`0xC00000` | |

Graphics use F3DEX/F3DLX/L3DEX 1.23 and S2DEX 1.03 (RT64 HLE). Audio is `n_aspMain`, the same build as
Mission: Impossible's, recompiled with RSPRecomp.

libultra was named by hand in `symbols/libultra.txt` from the call graph, hardware register use and
cache/COP0 instructions (for example, the `ll.c` 64-bit helpers match libultra's instruction counts one for one).

</details>

## Yes, AI wrote this

This port is being made by Claude (Anthropic's AI) working with TiJayFLY, who directs it and tests it. Claude mapped
the ROM, named the libultra functions and wrote the configs and app code.

It stands entirely on hand-built work by other people: [N64Recomp, N64ModernRuntime and Zelda64Recomp](https://github.com/N64Recomp)
by Mr-Wiseguy and contributors, [RT64](https://github.com/rt64/rt64) by Dario and contributors,
[RecompFrontend](https://github.com/N64Recomp/RecompFrontend) and [BanjoRecomp](https://github.com/BanjoRecomp/BanjoRecomp),
[splat](https://github.com/ethteck/splat) and [spimdisasm](https://github.com/Decompollaborate/spimdisasm),
[n64sym](https://github.com/shygoo/n64sym), and the N64 decomp scene. Thank you.

## License

GPL-3.0, see [COPYING](COPYING). GT 64: Championship Edition is © 1998 Imagineer / Ocean; this project contains none
of its code or data.
