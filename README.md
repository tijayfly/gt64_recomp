<div align="center">

# GT 64 Recompiled

**GT 64: Championship Edition (N64, 1998), as a native PC program.**
Gamepad support · any resolution · widescreen races · full-frame 4:3 screens

</div>

---

> [!WARNING]
> **Work in progress.** The boot logos, intro, menus and races are playable (tested on Windows; the Linux build
> runs headless). Saving isn't working yet. See [Status](#status).

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
- [x] Boots through the licence, logos, intro and title, with audio
- [x] Menus, championship and time trial setup, races, pause menu
- [x] HUD: rev counter needle and track map (rotated S2DEX sprites)
- [x] Skies, including the intro's split-screen views
- [x] Texture filtering as on the N64: sharp 2D (logos, menus, HUD), filtered sky
- [x] Gamepad and keyboard defaults for racing: triggers for throttle/brake, shoulders shift
- [x] Windows x64 build (cross-compiled, see [docs/WINDOWS_CROSS.md](docs/WINDOWS_CROSS.md))
- [x] Track seams: hairline cracks between polygons filled at high resolution, with or without MSAA
- [x] Custom app icon
- [ ] Saves (Controller Pak; the runtime reports no pak, so the game offers to continue without saving)
- [ ] The bottom rows of the race view can be left empty for a frame when the game culls the track piece under the
  camera (it only culled below its original window); they are filled from the rows above, a stopgap

## Enhancements

- **Widescreen races.** The race view fills the screen: the game draws its 3D in a 280x208 window inside a black
  border, which is widened to the full frame, and the sky wraps out to the window's edges. The HUD keeps its 4:3
  layout.
- **Full-frame 4:3 screens.** The logos, title and menus are laid out in a 288x208 box inside a black border. They
  are stretched to fill the 4:3 frame and pillarboxed. The switch between pillarbox and widescreen happens on black
  frames, so going into and out of races is clean.
- **Softer car shadows.** Filtered and blended at about half strength instead of solid black.
- **No seams in the track.** At high resolutions the sky showed through hairline gaps between track polygons; a pass
  after each 3D frame fills gaps up to an N64 pixel wide with the colour around them (works with MSAA).
- **Start lights at the top of the screen.** They hung from the top of the old window.
- **Any resolution.** RT64 renders at the window's resolution.
- **30 FPS, like the original.** The game times some things by frame, so races and the intro ran too fast at
  60 FPS, and RT64's frame interpolation flashed blue: the frame rate and refresh rate options are fixed to Original
  and hidden.

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

<details>
<summary>Game hooks and RT64 changes</summary>

Hooks in `gt64.us.toml` call native code in `src/game/enhancements.cpp`:

- the controller thread waits for each frame (reads finish instantly here, so it starved the other threads);
- the ROM read routine loads and unloads the front-end overlay;
- the camera routine (`func_8000A7DC`) widens the single-view scissor rect to the full frame and scales the menus'
  car models to match their stretched 2D;
- on every VI, the game mode (`0x800E8810`) picks widescreen or pillarbox and the 2D box to stretch.

Patches to RT64 and N64ModernRuntime are in `patches/deps/` and applied by CMake:

- `rt64-s2dex1-objects.patch`: S2DEX 1.x object commands (rectangles, sprites, rotated sprites, LOADTXTR), 4/8-bit
  `bgCopy` and `bg1Cyc` backgrounds, the sky wrapped out to the window's edges, the fade grid covering the frame,
  point-sampled sprites;
- `rt64-full-frame-view.patch`: the 2D remap ("card box" to the full frame, in 2D tasks only), whole-pixel edges,
  one-pixel-short scissors widened (a line at the right edge), no gaps between wrapped rectangles, the softer car
  shadows, flags the game sets (full-frame view, race);
- `rt64-raw-triangles-aspect.patch`: screen-space triangles keep their 4:3 place in widescreen; filtered sky draws;
- `rt64-crack-fill.patch`: the crack fill pass (`CrackFillPS.hlsl`, with MSAA variants) and the race view's
  bottom-row fill. Patches that touch the same files are made with `scripts/rt64_patch_diff.sh`;
- `N64ModernRuntime-screen-update-after-vi-swap.patch`: present after the VI swap (flicker);
- the rest are build and platform fixes shared with Mission: Impossible Recompiled.

</details>

<details>
<summary>Testing</summary>

`scripts/headless.sh` runs the game under Xvfb (lavapipe works) and takes screenshots; `GT_INPUT=<file>` feeds
controller 1 from a file. `scripts/torace.sh <dir>` drives a run from boot into a Free Run race. Debug switches:
`GT_MODELOG` (game modes), `GT_VPLOG` (camera view), `GT_TASKLOG`, `GT_STATS`, `GT_NO_REMAP` (turn off the 2D
stretch), `GT_CARD_BOX_Y=y0,y1` (override the 2D box), `GT_NO_CRACK_FILL` / `GT_CRACK_FILL_DEBUG` (turn off the crack
fill / show what it fills in magenta), `GT_NO_SHADOW_FIX`.

</details>

## AI Disclosure and Notes

This port is being made by Claude (Anthropic's AI) working with TiJayFLY, who directs it and tests it. Claude mapped
the ROM, named the libultra functions, wrote the configs and app code and the RT64 changes, and tested headless;
TiJayFLY tested every build on Windows and reported what was wrong.

It stands entirely on hand-built work by other people: [N64Recomp, N64ModernRuntime and Zelda64Recomp](https://github.com/N64Recomp)
by Mr-Wiseguy and contributors, [RT64](https://github.com/rt64/rt64) by Dario and contributors,
[RecompFrontend](https://github.com/N64Recomp/RecompFrontend) and [BanjoRecomp](https://github.com/BanjoRecomp/BanjoRecomp),
[splat](https://github.com/ethteck/splat) and [spimdisasm](https://github.com/Decompollaborate/spimdisasm),
[n64sym](https://github.com/shygoo/n64sym), and the N64 decomp scene. Thank you.

## License

GPL-3.0, see [COPYING](COPYING). GT 64: Championship Edition is © 1998 Imagineer / Ocean; this project contains none
of its code or data.
