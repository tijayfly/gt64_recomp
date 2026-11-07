<div align="center">

# GT 64 Recompiled

**GT 64: Championship Edition (N64, 1998), as a native PC program.**

Gamepad support · any resolution · uncapped framerate · widescreen races · extended draw distance

</div>

---

> [!WARNING]
> **Work in progress.** The boot logos, intro, menus, races, replays and saves work (tested on Windows; the Linux
> build runs headless). See [Status](#status).

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
- [x] Plays through the licence, logos, intro, title, menus and races, with audio
- [x] Championship and time trial setup, pause menu; logos and menus unfiltered, as on the N64
- [x] Replays, including runs driven with analog pedals (the pedal amounts are kept for the session, so a replay
  from before a restart plays back with on/off pedals)
- [x] HUD, unfiltered: rev counter needle and track map (rotated S2DEX sprites); start lights placed for the
  full-frame race view
- [x] Skies, filtered, including the intro's split-screen views
- [x] Gamepad and keyboard defaults for racing: triggers for throttle/brake (analog), shoulders shift
- [x] Windows x64 build (cross-compiled, see [docs/WINDOWS_CROSS.md](docs/WINDOWS_CROSS.md))
- [x] Track seams: cracks between polygons filled at high resolution, with or without MSAA, including the wider
  ones close to the camera and in replays. A few larger gaps in the track models are left as they are (they're in
  the original too)
- [x] Custom app icon
- [x] Rumble: strength in General > Rumble Strength (0 turns it off)
- [x] Saves: each controller has a Controller Pak and a Rumble Pak at once, so there's nothing to swap: saving and
  loading use the Controller Pak, races rumble. The pak is a standard 32 KB `.mpk` image (`controller_pak_1.mpk`) in
  the saves folder, so emulator pak files can be dropped in

## Enhancements

- **Widescreen races.** The race view fills the screen: the game draws its 3D in a 280x208 window inside a black
  border, which is widened to the full frame, and the sky wraps out to the window's edges. The HUD keeps its 4:3
  layout.
- **Full-frame 4:3 screens.** The logos, title and menus are laid out in a 288x208 box inside a black border. They
  are stretched to fill the 4:3 frame and pillarboxed. The switch between pillarbox and widescreen happens on black
  frames, so going into and out of races is clean.
- **Analog throttle and brake.** With General > Analog Triggers on (the default), the throttle and brake follow how
  far the triggers (or any analog input bound to A and B) are pressed, on top of the game's own pedal ramp.
- **Custom automatic gearbox.** General > Automatic Gearbox: *Custom* shifts up above 90% of the rev limit and down
  below 55% (never into neutral or reverse), for cars driven from a controller. *Original* (the default) keeps each
  car's own shift points.
- **Draw distance.** Graphics > Draw Distance, three levels:
  - *Maximum* (default): the whole track, every car, shadow and piece of trackside scenery, at any distance. Split
    screen (the intro) uses *Extended*, as the whole track for each view is too slow.
  - *Extended*: twice as much track ahead of the camera (20 sections instead of about 11) and more behind, with cars,
    shadows and scenery drawn as far as the track reaches.
  - *Original*: the game's own ranges.

  Extended and Maximum also draw the most detailed track models at any distance, move the far plane out with fog and
  weather left where they were, and draw scenery at the edges of the widescreen view, which the game culled with a
  view cone sized for 4:3.
- **Softer car shadows.** Filtered and blended at about half strength instead of solid black.
- **Any resolution.** RT64 renders at the window's resolution.
- **Uncapped framerate.** Graphics > Refresh Rate: *Display* runs races at the display's refresh rate (60, 120, 144 Hz
  and up), *Manual* at a rate you set, *Original* at the game's 30 FPS. The extra frames are drawn between the game's
  own, so the game itself, its physics and replay playback keep their original timing. Replays and the intro stay
  at 30 FPS whatever the setting.

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
- the car input routine (`func_80033050`) and the throttle and brake pedal updates (`func_8002D8B4`,
  `func_8002DB18`): analog triggers cap the game's pedal positions at how far the triggers are pressed;
- the controller thread's replay record and playback (`func_800081F0`): replays store only buttons and stick, so
  the pedal amounts are kept with each recorded entry and applied again when it's played back;
- the race view's track list (`func_8000A350`, `func_8000A280`, `func_8000A570`): more track sections, with list nodes
  and matrices past the game's buffers in the Expansion Pak's free 4 MB and the streamed track geometry served from a
  copy of its ROM block there;
- the pak probes (`osPfsInitPak`): the one at boot (`func_80043CB4`) answers as a Rumble Pak, so the game rumbles;
  the one before each save or load (`func_8004447C`) finds the Controller Pak and keeps the rumble flag it would
  clear;
- on every VI, the game mode (`0x800E8810`) picks widescreen or pillarbox (re-applied if a settings change brings
  back the saved aspect ratio) and the 2D box to stretch.

Patches to RT64 and N64ModernRuntime are in `patches/deps/` and applied by CMake:

- `N64ModernRuntime-controller-pak.patch`: the Controller Pak calls (`osPfs*`), which only ever answered "no pak",
  backed by a 32 KB pak image per controller.
- `rt64-s2dex1-objects.patch`: S2DEX 1.x object commands (rectangles, sprites, rotated sprites, LOADTXTR), 4/8-bit
  `bgCopy` and `bg1Cyc` backgrounds, the sky wrapped out to the window's edges, the fade grid covering the frame,
  point-sampled sprites, the sky drawn filtered;
- `rt64-full-frame-view.patch`: the 2D remap ("card box" to the full frame, in 2D tasks only), whole-pixel edges,
  one-pixel-short scissors widened (a line at the right edge), no gaps between wrapped rectangles, the softer car
  shadows, the credits' window box dropped, flags the game sets (full-frame view, race);
- `rt64-raw-triangles-aspect.patch`: screen-space triangles keep their 4:3 place in widescreen;
- `rt64-draw-distance.patch`: the race camera's far plane moved out (with its fog remapped to stay put) and its depth
  branches always taken.
- `rt64-crack-fill.patch`: the crack fill pass (`CrackFillPS.hlsl`, with MSAA variants): gaps up to one N64 pixel,
  up to three close to the camera where only the sky shows through, with the depth margin scaled for distant
  views; and the race view's bottom-row fill. Patches that touch the same files are made with `scripts/rt64_patch_diff.sh`;
- `rt64-frame-interpolation.patch`: frame interpolation for a game that draws each frame in several tasks (clear,
  sky, scene, player car, HUD): a frame's workloads are held until it's shown and rendered and interpolated as one,
  scenes are matched within their own task, transforms by their position relative to the camera, and camera cuts
  (a sharp turn or zoom in one frame) aren't interpolated;
- `N64ModernRuntime-screen-update-after-vi-swap.patch`: present after the VI swap (flicker);
- the rest are build and platform fixes shared with Mission: Impossible Recompiled.

</details>

<details>
<summary>Testing</summary>

`scripts/headless.sh` runs the game under Xvfb (lavapipe works) and takes screenshots; `GT_INPUT=<file>` feeds
controller 1 from a file. `scripts/torace.sh <dir>` drives a run from boot into a Free Run race. Debug switches:
`GT_MODELOG` (game modes), `GT_VPLOG` (camera view), `GT_TASKLOG`, `GT_STATS`, `GT_NO_REMAP` (turn off the 2D
stretch), `GT_CARD_BOX_Y=y0,y1` (override the 2D box), `GT_NO_CRACK_FILL` / `GT_CRACK_FILL_DEBUG` (turn off the crack
fill / show what it fills in magenta), `GT_DEPTH_VIEW` (show depth as grey, for tuning the fill), `GT_NO_SHADOW_FIX`,
`GT_KEEP_WINDOW_BOX` (keep the credits' translucent box), `GT_PEDAL_TEST=<0..1>` (stand-in trigger amount while A is
held) / `GT_PEDAL_LOG` (pedal and speed), `GT_REPLAY_LOG` (replay pedal recording and playback), `GT_RUMBLE_LOG`
(pak probes, motor on and off), `GT_PAK_LOG` (Controller Pak calls and save requests), `GT_CULL_LOG` (Extended's object reach per track), `GT_AR_RESET_TEST` (re-apply the saved aspect ratio every 5 s). `GT_INTERP_HALF=<0..1>` draws every frame that far back
toward the previous one (tests the frame matching headless, where the refresh rate can't be raised), `GT_CUT_LOG`
(camera cuts left uninterpolated), `GT_INTERP_LOG` (once a second: frames matched and interpolated, matching time and
how long RT64's threads waited on each other).

On Windows the game's output goes to `GT64Recompiled.log` beside the exe. A crash adds the exception, the crashing
thread and its call chain (module + offset, unwound from the exception) there, and writes `GT64Recompiled.dmp`.

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
