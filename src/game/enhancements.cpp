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

// Widescreen and full-height 4:3 screens.
// - The boot logos (licence, GT, Ocean, Imagineer) and the intro are 4:3 pictures whose full-screen fills reach past
//   the 4:3 frame with RT64's "Expand": while the game is in those modes, render at the original 4:3 aspect ratio
//   (pillarboxed) and switch back afterwards. Only the runtime's live setting changes; the saved setting is untouched.
// - The logos, title and menus are drawn inside a 16-line border (y 16..224): zoom the output by 240/208 so they fill
//   the screen's height. The race and intro views fill the frame (see gt_race_view) and are not zoomed.
#include "ultramodern/config.hpp"
#include "gt_game.h"

namespace RT64 { void setVIZoom(float zoom); void setFullFrameView(bool enabled); }

namespace {
    constexpr uint32_t mode_func_ptr = 0x800E8810;
    constexpr uint32_t generic_mode = 0x800018A8;  // between modes: keep the previous state
    constexpr uint32_t logo_modes[] = { 0x8000311C, 0x8000313C, 0x80003388, 0x800033D0, 0x80003400, 0x80003458, 0x800034A0, 0x800034F8 };
    constexpr uint32_t intro_mode = 0x80003540;
    constexpr uint32_t menu_modes[] = {
        // Title and attract, mode select, the front-end menus (team/car select, setup) and the round menu.
        0x80003640, 0x800036A8, 0x80003700, 0x80003760, 0x80003810, 0x80003878, 0x800038D0, 0x8000391C, 0x800039CC,
        0x80003BB4, 0x80004124, 0x8000416C,
    };
    constexpr float border_zoom = 240.0f / 208.0f;
    bool pillarboxed = false;
    bool zoomed = false;

    template <size_t N> bool in(const uint32_t (&list)[N], uint32_t v) {
        return std::find(std::begin(list), std::end(list), v) != std::end(list);
    }
}

void gt::widescreen_on_vi() {
    uint8_t* rdram = gt::game_rdram();
    if (rdram == nullptr) {
        return;
    }
    const uint32_t mode = *reinterpret_cast<uint32_t*>(rdram + (mode_func_ptr - 0x80000000u));
    static uint32_t last_mode = 0;
    if (mode != last_mode) {
        static const bool log = std::getenv("GT_MODELOG") != nullptr;
        if (log) fprintf(stderr, "[mode] %08X t=%.1f\n", mode, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        last_mode = mode;
    }
    if (mode == generic_mode) {
        return;
    }

    const bool logo = in(logo_modes, mode);
    const bool pillar = logo || (mode == intro_mode);
    const bool zoom = logo || in(menu_modes, mode);

    if (zoom != zoomed) {
        static const bool no_zoom = std::getenv("GT_NO_ZOOM") != nullptr;
        RT64::setVIZoom((zoom && !no_zoom) ? border_zoom : 1.0f);
        zoomed = zoom;
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
}
