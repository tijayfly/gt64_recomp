// Native helpers called from recompiler hooks (gt64.us.toml) to implement enhancements.
#include <cstdint>

#include "gt_config.h"

extern "C" int gt_fps60_enabled() {
    return gt::get_frame_rate() == gt::FrameRate::Fps60;
}

#include "ultramodern/ultra64.h"
#include "recomp.h"

// Controller thread (func_800083F8): on the N64 each controller read takes real SI time, which paced its polling loop.
// Here reads complete instantly, so the loop never blocks and starves the other (cooperatively scheduled) threads.
// Wait for the next frame message on the thread's own VI client queue (at sp+0x70) before each poll.
extern "C" void gt_controller_wait(uint8_t* rdram, recomp_context* ctx) {
    osRecvMesg(rdram, (PTR(OSMesgQueue))(int32_t)(ctx->r29 + 0x70), NULLPTR, OS_MESG_BLOCK);
}

// Front-end code overlay (ROM 0xC7920, loaded at 0x801A2520). The runtime only registers overlays at boot, so the
// game's ROM read routine (func_80001A24: a0 = ROM, a1 = RAM, a2 = size) tells it when the overlay is loaded, and when
// other data (the race's) is read over it.
extern "C" void load_overlays(uint32_t rom, int32_t ram_addr, uint32_t size);
extern "C" void unload_overlays(int32_t ram_addr, uint32_t size);

namespace {
    constexpr uint32_t ovl_front_rom = 0xC7920;
    constexpr uint32_t ovl_front_ram = 0x801A2520;
    constexpr uint32_t ovl_front_text_size = 0xE57A0 - 0xC7920;
    bool ovl_front_loaded = false;
}

extern "C" void gt_on_rom_read(uint32_t rom, uint32_t ram, uint32_t size) {
    bool overlaps = ram < ovl_front_ram + ovl_front_text_size && ram + size > ovl_front_ram;
    if (rom == ovl_front_rom && ram == ovl_front_ram) {
        if (!ovl_front_loaded) {
            load_overlays(ovl_front_rom, (int32_t)ovl_front_ram, size);
            ovl_front_loaded = true;
        }
    }
    else if (overlaps && ovl_front_loaded) {
        unload_overlays((int32_t)ovl_front_ram, ovl_front_text_size);
        ovl_front_loaded = false;
    }
}

#include <cstdio>

// Debug: count RSP task types submitted from each site (GT_TASKLOG=1).
#include <chrono>
#include <cstdlib>
#include <algorithm>
extern "C" void gt_debug_task(uint8_t* rdram, uint32_t task, int site) {
    static const bool on = std::getenv("GT_TASKLOG") != nullptr;
    if (!on) return;
    static int counts[3][8] = {};
    static auto last = std::chrono::steady_clock::now();
    uint32_t type = *reinterpret_cast<uint32_t*>(rdram + (task - 0x80000000u));
    counts[site][type & 7]++;
    static const bool all = std::getenv("GT_TASKLOG")[0] == '2';
    if (site == 0 && (all || counts[0][1] % 60 == 1)) {
        auto w = [&](uint32_t off) { return *reinterpret_cast<uint32_t*>(rdram + (task + off - 0x80000000u)); };
        fprintf(stderr, "[gfxtask] ucode=%08X data_ptr=%08X flags=%08X sc_flags=%08X fb=%08X\n", w(0x10), w(0x30), w(0x4), w(-0x28 + 0x1C), w(-0x28 + 0x20));
    }
    auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::seconds(1)) {
        last = now;
        for (int s = 0; s < 3; s++) fprintf(stderr, "[task] site%d gfx=%d aud=%d other=%d\n", s, counts[s][1], counts[s][2], counts[s][0] + counts[s][3] + counts[s][4]);
        std::fill(&counts[0][0], &counts[0][0] + 24, 0);
    }
}

extern "C" void osViBlack(uint8_t active);
extern "C" void gt_debug_swap(uint32_t fb) {
    static const bool unblack = std::getenv("GT_FORCE_UNBLACK") != nullptr;
    if (unblack) osViBlack(0);
    static const bool on = std::getenv("GT_TASKLOG") != nullptr;
    static int n = 0;
    static const bool all = std::getenv("GT_TASKLOG") && std::getenv("GT_TASKLOG")[0] == '2';
    if (on && (all || (n % 60) == 0)) fprintf(stderr, "[swap] n=%d fb=0x%08X\n", n, fb);
    n++;
}

// Widescreen and the 4:3 screens.
// - Races render with RT64's "Expand" (widescreen). Everything else (boot logos, intro, title, menus) is a 4:3 picture
//   and renders at the original aspect ratio, pillarboxed. Only the runtime's live setting changes; the saved setting
//   is untouched. A race is told apart by its full-frame 3D view (gt_race_view), so the switch happens when the race
//   starts to fade in from black and once it has faded out.
// - The logos, title and menus are laid out in a 288x208 box at (16, 16) inside a black border. While they are up, RT64
//   remaps the game's 2D tasks so the box fills the 4:3 frame (and the menus' 3D car models are scaled to match), but
//   not while the intro's split-screen views are drawn (their windows would no longer line up with their 3D).
#include "ultramodern/config.hpp"
#include "gt_game.h"

namespace RT64 { void setCardRemap(bool enabled); void setFullFrameView(bool enabled); void setCardRemapBox(int32_t x0, int32_t y0, int32_t x1, int32_t y1); }

namespace {
    constexpr uint32_t mode_func_ptr = 0x800E8810;
    constexpr uint32_t generic_mode = 0x800018A8;  // between modes: keep the previous state
    constexpr uint32_t intro_mode = 0x80003540;
    // Modes from the round menu's exit on (pre-race, loading, the race, its pause menu, results...): widescreen while
    // they draw a race (a full-frame 3D view).
    constexpr uint32_t race_modes_start = 0x80004240;
    uint32_t full_view_vi = 0;  // last VI on which a full-frame 3D view was built
    bool pillarboxed = false;
    bool card_remap = false;
    uint32_t vi_count = 0;
    uint32_t split_view_vi = 0;  // last VI on which a split (not full-frame) 3D view was built
    bool menu_3d_scale = false;  // read by gt_race_view

    template <size_t N> bool in(const uint32_t (&list)[N], uint32_t v) {
        return std::find(std::begin(list), std::end(list), v) != std::end(list);
    }
}

void gt::widescreen_on_vi() {
    uint8_t* rdram = gt::game_rdram();
    if (rdram == nullptr) {
        return;
    }
    vi_count++;
    const uint32_t mode = *reinterpret_cast<uint32_t*>(rdram + (mode_func_ptr - 0x80000000u));
    static uint32_t last_mode = 0;
    if (mode != last_mode) {
        static const bool log = std::getenv("GT_MODELOG") != nullptr;
        if (log) fprintf(stderr, "[mode] %08X t=%.1f\n", mode, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        last_mode = mode;
    }
    // Boot (before the first logo) and the generic in-between mode keep the current state.
    if (mode == generic_mode || mode < 0x8000311C) {
        return;
    }

    // Frames are built at 30 FPS or less (much less while loading), so allow a couple of seconds between them.
    // The card box: the menus, title and intro use 288x208 at (16, 16); the boot logo cards end at line 216 (the GT and
    // Ocean cards' white starts at 16, the Imagineer picture at 8). The Imagineer card fades out over the start of the
    // intro, so keep its box for the intro's first seconds (the intro's own 3D isn't remapped, and nothing 2D is
    // drawn again until the title).
    {
        static uint32_t intro_vi = 0;
        static uint32_t prev_mode = 0;
        if (mode == intro_mode && prev_mode != intro_mode) {
            intro_vi = vi_count;
        }
        prev_mode = mode;
        int32_t y0 = 16, y1 = 224;
        if (mode >= 0x8000311C && mode <= 0x80003458) {
            y1 = 216;
        }
        else if (mode == 0x800034A0 || mode == 0x800034F8 || (mode == intro_mode && vi_count - intro_vi < 4 * 60)) {
            y0 = 8;
            y1 = 216;
        }
        static const char* box_env = std::getenv("GT_CARD_BOX_Y");  // debug: "y0,y1"
        if (box_env != nullptr) {
            sscanf(box_env, "%d,%d", &y0, &y1);
        }
        static int32_t box_y0 = 0, box_y1 = 0;
        if (y0 != box_y0 || y1 != box_y1) {
            RT64::setCardRemapBox(16, y0, 304, y1);
            box_y0 = y0;
            box_y1 = y1;
        }
    }

    const bool race = (mode >= race_modes_start) && (vi_count - full_view_vi <= 120);
    {
        static const bool log = std::getenv("GT_MODELOG") != nullptr;
        static int last = -1;
        if (log && int(race) != last) { fprintf(stderr, "[race] %d mode=%08X\n", int(race), mode); last = race; }
    }
    const bool pillar = !race;
    const bool remap = !race && (vi_count - split_view_vi > 2);
    menu_3d_scale = remap && (mode != intro_mode);

    if (remap != card_remap) {
        static const bool no_remap = std::getenv("GT_NO_REMAP") != nullptr;
        RT64::setCardRemap(remap && !no_remap);
        card_remap = remap;
    }

    using namespace ultramodern::renderer;
    const GraphicsConfig& current = get_graphics_config();
    if (pillar && !pillarboxed && current.ar_option == AspectRatio::Expand) {
        GraphicsConfig config = current;
        config.ar_option = AspectRatio::Original;
        set_graphics_config(config);
        pillarboxed = true;
    }
    else if (!pillar && pillarboxed) {
        if (current.ar_option == AspectRatio::Original) {
            GraphicsConfig config = current;
            config.ar_option = AspectRatio::Expand;
            set_graphics_config(config);
        }
        pillarboxed = false;
    }
}

// Race view (see gt64.us.toml).
extern "C" void gt_race_view(uint8_t* rdram) {
    auto w = [&](uint32_t a) -> uint32_t& { return *reinterpret_cast<uint32_t*>(rdram + (a - 0x80000000u)); };
    auto h = [&](uint32_t a) -> int16_t& { return *reinterpret_cast<int16_t*>(rdram + ((a ^ 2) - 0x80000000u)); };
    uint32_t cam = w(0x8010FFCC);
    if (cam < 0x80000000u || cam >= 0x80800000u) return;
    uint32_t vp = w(cam + 0xCC), rect = w(cam + 0xD0);
    static const bool log = std::getenv("GT_VPLOG") != nullptr;
    if (log) {
        static int n = 0;
        if (n++ % 120 == 0)
            fprintf(stderr, "[view] cam=%08X vp=%08X (%d %d %d %d / %d %d %d %d) rect=%08X (%d %d %d %d %d)\n", cam, vp,
                h(vp), h(vp + 2), h(vp + 4), h(vp + 6), h(vp + 8), h(vp + 10), h(vp + 12), h(vp + 14), rect,
                w(rect), w(rect + 4), w(rect + 8), w(rect + 12), w(rect + 16));
    }
    // The viewport is already the full 320x240 frame; only the scissor rect {mode, x0, y0, x1, y1} crops it.
    // Single-player window only: split-screen rects differ and stay as they are.
    if (rect < 0x80000000u || rect >= 0x80800000u) return;
    if (w(rect + 4) == 20 && w(rect + 8) == 16 && w(rect + 12) == 300 && w(rect + 16) == 224) {
        w(rect + 4) = 0; w(rect + 8) = 0; w(rect + 12) = 320; w(rect + 16) = 240;
    }
    // Tell RT64 whether the frame is one full view, so it draws the sky and fades out to the screen's edges too.
    const bool full = w(rect + 4) == 0 && w(rect + 8) == 0 && w(rect + 12) == 320 && w(rect + 16) == 240;
    RT64::setFullFrameView(full);
    if (!full) {
        split_view_vi = vi_count;
    }
    else if (w(mode_func_ptr) >= race_modes_start) {
        full_view_vi = vi_count;  // a race (the menus' car models use the same view)
    }
    // Menus: scale their 3D (car models) like the card remap scales the 2D around it: the viewport {scale, translate}
    // (s13.2: 160, 120 / 160, 120) to 320/288 x 240/208.
    if (vp >= 0x80000000u && vp < 0x80800000u) {
        const int16_t sx = menu_3d_scale ? 711 : 640, sy = menu_3d_scale ? 554 : 480;
        if ((h(vp) == 640 || h(vp) == 711) && (h(vp + 2) == 480 || h(vp + 2) == 554) && h(vp + 8) == 640 && h(vp + 10) == 480) {
            h(vp) = sx;
            h(vp + 2) = sy;
        }
    }
}
