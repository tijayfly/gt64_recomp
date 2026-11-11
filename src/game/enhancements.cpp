// Native helpers called from recompiler hooks (gt64.us.toml) to implement enhancements.
#include <cstdint>
#include <cstring>
#include <cmath>
#include "librecomp/game.hpp"
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdlib>

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
    uint32_t front_overlay_loads = 0;   // times the front-end overlay has been read in (leaving a race, among others)
    // The display list buffer being built (gt_matrix_buffer), and for each, whether it drew a race view while the
    // front-end overlay was in (over the race's data): a race frame built after leaving the race.
    uint32_t building_buffer = 0;
    bool ext_gfx_stale_race[2] = {};
}

extern "C" void gt_on_rom_read(uint32_t rom, uint32_t ram, uint32_t size) {
    if (rom == ovl_front_rom && ram == ovl_front_ram) {
        front_overlay_loads++;
    }
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
#include <unordered_map>
#include <cstdlib>
#include <algorithm>
extern "C" void gt_debug_task(uint8_t* rdram, uint32_t task, int site) {
    static const bool on = std::getenv("GT_TASKLOG") != nullptr;
    if (!on) return;
    if (std::getenv("GT_TASKLOG")[0] == '3') {
        static std::map<std::string, int> seen;
        auto w = [&](uint32_t off) { return *reinterpret_cast<uint32_t*>(rdram + (task + off - 0x80000000u)); };
        char key[160];
        std::snprintf(key, sizeof(key), "site%d type %u boot %08X/%X ucode %08X/%X data %08X/%X dl %05X..", site, w(0), w(8), w(0xC), w(0x10), w(0x14), w(0x18), w(0x1C), (w(0x30) & 0xFFFFFF) >> 12);
        if (seen[key]++ == 0) std::fprintf(stderr, "[taskseen] %s\n", key);
        return;
    }
    static int counts[3][8] = {};
    static auto last = std::chrono::steady_clock::now();
    uint32_t type = *reinterpret_cast<uint32_t*>(rdram + (task - 0x80000000u));
    counts[site][type & 7]++;
    static const bool all = std::getenv("GT_TASKLOG")[0] == '2';
    if (site == 0 && (all || counts[0][1] % 60 == 1)) {
        auto w = [&](uint32_t off) { return *reinterpret_cast<uint32_t*>(rdram + (task + off - 0x80000000u)); };
        const uint32_t dl = w(0x30) | 0x80000000u;
        auto r = [&](uint32_t a) { return (a >= 0x80000000u && a < 0x807FFFF8u) ? *reinterpret_cast<uint32_t*>(rdram + (a - 0x80000000u)) : 0xDEADBEEFu; };
        fprintf(stderr, "[gfxtask] ucode=%08X data_ptr=%08X flags=%08X fb=%08X dl %08X %08X %08X %08X wp=%08X idx=%u\n", w(0x10), w(0x30), w(0x4), w(-0x28 + 0x20),
            r(dl), r(dl + 4), r(dl + 8), r(dl + 12), r(0x8010FFC8u), unsigned(rdram[(0x80119597u ^ 3) - 0x80000000u]));
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

namespace RT64 { void setCardRemap(bool enabled); void setFullFrameView(bool enabled); void setCardRemapBox(int32_t x0, int32_t y0, int32_t x1, int32_t y1); void setRaceHoleFill(bool enabled); void setOutputBrightness(float brightness); void setFarScale(float scale); void setOriginalRateOnly(bool enabled); void setDofStart(float distance); void setMotionCamera(double x, double y, double z, bool valid); }
namespace {
    double section_spacing(uint8_t* rdram);
    extern int32_t original_forward;
}

namespace {
    constexpr uint32_t mode_func_ptr = 0x800E8810;
    constexpr uint32_t generic_mode = 0x800018A8;  // between modes: keep the previous state
    constexpr uint32_t intro_mode = 0x80003540;
    // Modes from the round menu's exit on (pre-race, loading, the race, its pause menu, results...): widescreen while
    // they draw a race (a full-frame 3D view).
    constexpr uint32_t race_modes_start = 0x80004240;
    uint32_t full_view_vi = 0;  // last VI on which a full-frame 3D view was built
    uint32_t track_views = 0;   // track ring views built (func_8000A350: races, replays, the intro and attract demo)
    bool pillarboxed = false;
    // The aspect ratio to go back to after a pillarboxed screen (the saved setting: Expand or Manual).
    ultramodern::renderer::AspectRatio widescreen_ar = ultramodern::renderer::AspectRatio::Expand;
    bool card_remap = false;
    uint32_t vi_count = 0;
    uint32_t split_view_vi = 0;  // last VI on which a split (not full-frame) 3D view was built
    bool menu_3d_scale = false;  // read by gt_race_view
    uint32_t race_view_frames = 0;  // full-frame race views built (by several cameras each frame)

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
    // The big title font's full stop (its glyph entry at D_8008DD90: width, atlas width, left column, flags, atlas, height)
    // starts a column early, on the 9's outline before it, and leaves its own outline's last column out: "U.S.A." titles
    // showed a grey bar before each stop. One column on.
    {
        uint16_t* left = reinterpret_cast<uint16_t*>(rdram + ((0x8008DD94u ^ 2) - 0x80000000u));
        const uint16_t width = *reinterpret_cast<uint16_t*>(rdram + ((0x8008DD90u ^ 2) - 0x80000000u));
        if (*left == 0x75 && width == 5) {
            *left = 0x76;
        }
    }
    // Game Over credits (front-end overlay page at 0x801C9860): two 0x50-byte lines, each u16 x, u16 y, pad, then the
    // text three times in 24-byte slots. Replaced once the overlay is loaded; the font is fixed-width, about 9 px a char.
    {
        auto byte_at = [rdram](uint32_t addr) -> uint8_t& { return reinterpret_cast<uint8_t&>(rdram[(addr ^ 3) - 0x80000000u]); };
        auto matches = [&](uint32_t addr, const char* s) {
            for (; *s; ++s, ++addr) if (byte_at(addr) != uint8_t(*s)) return false;
            return true;
        };
        auto set_line = [&](uint32_t line, uint16_t x, const char* text) {
            *reinterpret_cast<uint16_t*>(rdram + ((line ^ 2) - 0x80000000u)) = x;
            for (uint32_t slot = 0; slot < 3; ++slot) {
                const uint32_t base = line + 8 + slot * 24;
                for (uint32_t i = 0; i < 24; ++i) byte_at(base + i) = 0;
                for (uint32_t i = 0; text[i] && i < 23; ++i) byte_at(base + i) = uint8_t(text[i]);
            }
        };
        if (matches(0x801C9868u, " GT ASSOCIATION") && matches(0x801C98B8u, "IMAGINEER CO.,LTD.")) {
            set_line(0x801C9860u, 106, "PC PORT BY");
            set_line(0x801C98B0u, 115, "TIJAYFLY");
        }
    }
    const uint32_t mode = *reinterpret_cast<uint32_t*>(rdram + (mode_func_ptr - 0x80000000u));
    static uint32_t last_mode = 0;
    if (mode != last_mode) {
        static const bool log = std::getenv("GT_MODELOG") != nullptr;
        if (log) fprintf(stderr, "[mode] %08X t=%.2f\n", mode, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        last_mode = mode;
    }
    // Boot (before the first logo) and the generic in-between mode keep the current state.
    if (mode == generic_mode || mode < 0x8000311C) {
        return;
    }

    // Frames are built at 30 FPS or less (much less while loading), so allow a couple of seconds between them.
    // The card box: the menus, title and intro use 288x208 at (16, 16). The boot logo cards (GT, Ocean, Imagineer:
    // one mode, 800034A0, then 800034F8) are 288x208 pictures at (16, 8) whose content is lines 16..216. The Imagineer
    // card fades out over the start of the intro, so keep its box for the intro's first seconds (the intro's own 3D
    // isn't remapped, and nothing 2D is drawn again until the title).
    {
        static uint32_t intro_vi = 0;
        static uint32_t prev_mode = 0;
        if (mode == intro_mode && prev_mode != intro_mode) {
            intro_vi = vi_count;
        }
        prev_mode = mode;
        int32_t y0 = 16, y1 = 224;
        if (mode == 0x800034A0 || mode == 0x800034F8 || (mode == intro_mode && vi_count - intro_vi < 20 * 60)) {
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

    // Once a race is running, leaving the race and pause modes ends it: the game changes mode on the black frames
    // before the results and menus, so the switch back to 4:3 happens there instead of seconds later on the next
    // screen. (The pause menu stops building race views, so a short time-out would switch it back to 4:3.)
    constexpr uint32_t pause_mode = 0x80004940;
    static bool racing = false;
    const bool in_race_mode = (mode == 0x800046C4) || (mode == pause_mode);
    // After a race ends, stay out of race until the menus (a race view seen within the last two seconds would
    // otherwise count as a race again on the results screen).
    static bool race_ended = false;
    if (mode < race_modes_start) {
        race_ended = false;
    }
    // The fade into a race (see below).
    static int transition = 0;  // 0 none, 1 fading out, 2 black, 4 black after the switch, 3 fading in
    static uint32_t transition_vi = 0;
    static uint32_t transition_swap = 0;
    static bool race_seen = false;  // the race has started drawing since the fade began
    // The loading modes from the start card's end into the race.
    constexpr uint32_t card_end_mode = 0x800042E8;
    constexpr uint32_t race_mode = 0x800046C4;
    const bool loading = (mode >= card_end_mode) && (mode <= race_mode);
    bool race = (mode >= race_modes_start) && (vi_count - full_view_vi <= 120) && (!racing || in_race_mode) && !race_ended;
    // Leaving a race: the game is part-way through its fade to black when it changes mode, so fade the output the rest
    // of the way to black, switch to 4:3 there, and bring the output back while the game's screen is still black.
    constexpr uint32_t end_fade_vis = 8, end_hold_vis = 24, end_fade_in_vis = 10;
    static int end_transition = 0;  // 0 none, 1 fading out (still widescreen), 2 black and 4:3, 3 fading back in
    static uint32_t end_vi = 0;
    if (racing && !race && end_transition == 0) {
        race_ended = true;
        end_transition = 1;
        end_vi = vi_count;
    }
    const uint32_t te = vi_count - end_vi;
    if (end_transition == 1) {
        RT64::setOutputBrightness(1.0f - float(te) / end_fade_vis);
        if (te >= end_fade_vis) {
            end_transition = 2;
        }
        else {
            race = true;  // keep widescreen until black
        }
    }
    if (end_transition == 2) {
        RT64::setOutputBrightness(0.0f);
        if (te >= end_fade_vis + end_hold_vis) {
            end_transition = 3;
        }
    }
    else if (end_transition == 3) {
        const uint32_t tf = te - end_fade_vis - end_hold_vis;
        RT64::setOutputBrightness(float(tf) / end_fade_in_vis);
        if (tf >= end_fade_in_vis) {
            end_transition = 0;
            RT64::setOutputBrightness(1.0f);
        }
    }
    // Restarting a Time Trial goes from the pause menu straight back into loading, not through the menus: a new race is
    // on its way. Drop the ended race's fade and go into the new one through black like any other race start.
    if (race_ended && loading && mode != race_mode) {
        race_ended = false;
        end_transition = 0;
        race = false;
        transition = 2;
        transition_vi = vi_count;
        race_seen = false;
        RT64::setOutputBrightness(0.0f);
    }
    {
        static const bool log = std::getenv("GT_MODELOG") != nullptr;
        static int last = -1;
        if (log && int(race) != last) { fprintf(stderr, "[race] %d mode=%08X t=%.2f\n", int(race), mode, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()); last = race; }
    }
    RT64::setRaceHoleFill(race);
    // Draw distance: while the track is being drawn (races, and also the intro and attract demo, which aren't race
    // modes), move the camera's far plane out and always take the depth branches (the track sections' LOD and distance
    // cut-off); see gt_sections_ahead.
    static uint32_t last_track_views = 0, track_view_vi = 0;
    if (track_views != last_track_views) {
        last_track_views = track_views;
        track_view_vi = vi_count;
    }
    const bool track_drawn = (track_view_vi != 0) && (vi_count - track_view_vi <= 4);
    RT64::setFarScale((track_drawn && gt::get_draw_distance() != gt::DrawDistance::Original) ? 16.0f : 1.0f);
    // Depth of field: past where the original draw distance would end facing forward (the game's own sections ahead), the
    // picture blurs, facing backward too.
    float dof_start = 0.0f;
    // (A looser test than track_drawn: RT64 only applies it to frames drawn with the extended far plane anyway, and a
    // slow frame shouldn't make it blink.)
    if ((track_view_vi != 0) && (vi_count - track_view_vi <= 60) && gt::get_depth_of_field() == gt::DepthOfField::On && gt::get_draw_distance() != gt::DrawDistance::Original) {
        dof_start = float(section_spacing(rdram) * original_forward);
    }
    RT64::setDofStart(dof_start);
    if (!track_drawn) {
        RT64::setMotionCamera(0.0, 0.0, 0.0, false);
    }
    // Into a race, the game cross-fades from the last 4:3 frame (the start card) to the race, so switching the aspect
    // ratio as the race appears stretched the card for a moment. Fade the output through black instead: fade out
    // (still pillarboxed) as the card ends, hold black through loading until the race mode has begun and the
    // cross-fade is over, switch, fade in.
    // The cross-fade lasts about 20 game frames: hold for 24 frames from the first race frame (and at least ~0.5 s).
    constexpr uint32_t fade_out_vis = 24, hold_min_vis = 24, hold_max_vis = 300, fade_in_vis = 15, crossfade_frames = 24;
    // Start fading out as the start card ends (the loading modes after it), before the game starts its cross-fade.
    if (((!race && !loading) || race_ended) && transition != 0) {
        transition = 0;
        RT64::setOutputBrightness(1.0f);
    }
    if ((race || loading) && pillarboxed && transition == 0 && !race_ended) {
        transition = 1;
        transition_vi = vi_count;
        race_seen = false;
    }
    // Game frames with a race view: gt_race_view runs for several cameras per frame, so count the VIs on which it ran.
    static uint32_t last_race_view_count = 0;
    static uint32_t race_frames = 0;
    if (race_view_frames != last_race_view_count) {
        last_race_view_count = race_view_frames;
        race_frames++;
    }
    if (transition != 0 && race && !race_seen) {
        race_seen = true;
        transition_swap = race_frames;
    }
    const uint32_t t = vi_count - transition_vi;
    if (transition == 1) {
        RT64::setOutputBrightness(1.0f - float(t) / fade_out_vis);
        if (t >= fade_out_vis) {
            transition = 2;
            transition_vi = vi_count;
        }
    }
    else if (transition == 2) {
        RT64::setOutputBrightness(0.0f);
        if ((race_seen && mode == race_mode && t >= hold_min_vis && race_frames - transition_swap >= crossfade_frames) ||
            (race_seen && t >= hold_max_vis)) {
            // Switch the aspect ratio while black, and wait for a few frames drawn in widescreen before fading in (the
            // frames already drawn are 4:3).
            transition = 4;
            transition_vi = vi_count;
            transition_swap = race_frames;
        }
    }
    else if (transition == 4) {
        RT64::setOutputBrightness(0.0f);
        if (race_frames - transition_swap >= 3 || t >= 60) {
            transition = 3;
            transition_vi = vi_count;
        }
    }
    else if (transition == 3) {
        RT64::setOutputBrightness(float(t) / fade_in_vis);
        if (t >= fade_in_vis) {
            transition = 0;
            RT64::setOutputBrightness(1.0f);
        }
    }
    racing = race && (transition == 0) && !pillarboxed;
    const bool pillar = !race || transition == 1 || transition == 2;  // (loading modes aren't races, so they pillarbox)
    const bool remap = !race && (vi_count - split_view_vi > 2);
    menu_3d_scale = remap && (mode != intro_mode);

    if (remap != card_remap) {
        static const bool no_remap = std::getenv("GT_NO_REMAP") != nullptr;
        RT64::setCardRemap(remap && !no_remap);
        card_remap = remap;
    }

    using namespace ultramodern::renderer;
    // Test: GT_AR_RESET_TEST re-applies the widescreen aspect ratio every 300 VIs, as the settings menu does.
    static const bool ar_reset_test = std::getenv("GT_AR_RESET_TEST") != nullptr;
    if (ar_reset_test && (vi_count % 300) == 0) {
        GraphicsConfig config = get_graphics_config();
        config.ar_option = AspectRatio::Expand;
        set_graphics_config(config);
    }
    const GraphicsConfig& current = get_graphics_config();
    // Checked on every VI, not only on mode changes: the settings menu re-applies the saved graphics settings (e.g. after
    // changing any option), which brought back the widescreen aspect ratio on a pillarboxed screen.
    if (pillar && current.ar_option != AspectRatio::Original) {
        GraphicsConfig config = current;
        widescreen_ar = current.ar_option;
        config.ar_option = AspectRatio::Original;
        set_graphics_config(config);
        pillarboxed = true;
    }
    else if (!pillar && pillarboxed) {
        if (current.ar_option == AspectRatio::Original) {
            GraphicsConfig config = current;
            config.ar_option = widescreen_ar;
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
        static uint64_t last = 0;
        const uint64_t key = (uint64_t(cam) << 32) ^ w(rect + 4) ^ (uint64_t(w(rect + 8)) << 8) ^ (uint64_t(w(rect + 12)) << 16) ^ (uint64_t(w(rect + 16)) << 24) ^ (uint64_t(uint16_t(h(vp))) << 40) ^ (uint64_t(uint16_t(h(vp + 2))) << 48) ^ (uint64_t(uint16_t(h(vp + 10))) << 20) ^ vp ^ (uint64_t(rect) << 12);
        if (key != last && (last = key, true))
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
        race_view_frames++;
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

// Analog throttle and brake (General > Analog Triggers).
// The game's per-frame car input routine (func_80033050) sets the car's throttle and brake flags (car +0x200, +0x204:
// 0 or 0x100) from the A and B bits of its controller's data. The car update then treats each flag as on/off and
// ramps a pedal position (doubles at car +0x180 throttle, +0x188 brake) towards 1.0 while it's on. With analog
// triggers, the input hook records how far the inputs bound to A and B are pressed, and the pedal updates
// (func_8002D8B4, func_8002DB18) are followed by capping the pedal position at that amount, so the game's own ramp
// still applies but stops where the trigger is.
#include "recompinput/recompinput.h"
#include "recompinput/profiles.h"
#include "recompinput/players.h"
#include "recompinput/input_state.h"

namespace {
    // How far the inputs bound to an N64 button are pressed (0..1), on a controller and the keyboard.
    float n64_button_analog(int player, recompinput::GameInput input) {
        using namespace recompinput;
        if (game_input_disabled()) {
            return 0.0f;
        }
        const bool sp = players::is_single_player_mode();
        const int profiles_for_player[2] = {
            sp ? profiles::get_sp_controller_profile_index() : profiles::get_input_profile_for_player(player, InputDevice::Controller),
            sp ? profiles::get_sp_keyboard_profile_index() : profiles::get_input_profile_for_player(player, InputDevice::Keyboard),
        };
        float value = 0.0f;
        for (int profile : profiles_for_player) {
            if (profile < 0) {
                continue;
            }
            for (size_t i = 0; i < num_bindings_per_input; i++) {
                value = std::max(value, get_input_analog(player, profiles::get_input_binding(profile, input, i)));
            }
        }
        return std::clamp(value, 0.0f, 1.0f);
    }

    constexpr float pedal_deadzone = 0.04f;

    float pedal_amount(float value) {
        return (value <= pedal_deadzone) ? 0.0f : std::min((value - pedal_deadzone) / (1.0f - pedal_deadzone), 1.0f);
    }

    // Pedal caps for cars driven by a live controller this frame (car address, throttle, brake).
    struct PedalCap {
        uint32_t car = 0;
        float throttle = 1.0f;
        float brake = 1.0f;
        bool live = false;  // driven from the controller now (not played back from a replay's recording)
    };
    PedalCap pedal_caps[4];
    uint32_t pedal_car = 0;  // car whose pedal is being updated

    void cap_pedal(uint8_t* rdram, uint32_t addr, float cap) {
        uint32_t& hi = *reinterpret_cast<uint32_t*>(rdram + (addr - 0x80000000u));
        uint32_t& lo = *reinterpret_cast<uint32_t*>(rdram + (addr + 4 - 0x80000000u));
        uint64_t bits = (uint64_t(hi) << 32) | lo;
        double value;
        memcpy(&value, &bits, sizeof(value));
        if (value > double(cap)) {
            value = double(cap);
            memcpy(&bits, &value, sizeof(bits));
            hi = uint32_t(bits >> 32);
            lo = uint32_t(bits);
        }
    }
}

// Replays: the controller thread (func_800081F0) records each port's pad into a buffer (4 bytes per poll: buttons,
// stick x, y; pointer at D_800E8844) and plays it back from there, so a replay only has the on/off A and B bits and
// drifted from a race driven with analog pedals. Keep the pedal amounts the race used alongside it, keyed by the buffer
// entry the game read that frame (with the entry's contents, so a different recording at the same place isn't used),
// and apply them again when that entry is played back.
namespace {
    enum class PadSource { Live, Recording, Playback };
    struct PadEntryUse {
        PadSource source = PadSource::Live;
        uint32_t entry = 0;
        uint32_t poll = 0;
    };
    PadEntryUse pad_entry_use[2];
    uint32_t controller_polls = 0;
    struct RecordedPedals {
        uint32_t contents;
        float throttle;
        float brake;
    };
    std::map<uint32_t, RecordedPedals> recorded_pedals;

    uint32_t read_word(uint8_t* rdram, uint32_t addr) {
        return *reinterpret_cast<uint32_t*>(rdram + (addr - 0x80000000u));
    }
}

// Paks. The game probes each controller's pak with osPfsInitPak: at boot (func_80043CB4) to find a Rumble Pak (if the
// answer is "not a Controller Pak", PFS_ERR_ID_FATAL / PFS_ERR_DEVICE, it tries osMotorInit and then rumbles through
// osMotorStart/Stop, which the runtime turns into controller rumble), and before each save or load (func_8004447C) to
// find the Controller Pak. The runtime has a Controller Pak in every controller (librecomp pak.cpp), so the boot probe is
// answered as a Rumble Pak and the save probe left as it is; that one clears the rumble flag, which its hook keeps.
extern "C" int32_t gt_pak_probe(int32_t port, int32_t result) {
    constexpr int32_t pfs_err_device = 11;
    static const bool log = std::getenv("GT_RUMBLE_LOG") != nullptr;
    if (log) {
        std::printf("[rumble] boot pak probe port %d: %d -> %d\n", port, result, pfs_err_device);
    }
    return pfs_err_device;
}

extern "C" void gt_rumble_log(int on) {
    static const bool log = std::getenv("GT_RUMBLE_LOG") != nullptr;
    if (log) {
        std::printf("[rumble] motor %s\n", on ? "start" : "stop");
    }
}

extern "C" void gt_controller_poll(uint8_t* rdram) {
    controller_polls++;

    // Replays (port 1's pad played back from the recording) and the intro keep the game's own 30 FPS: the renderer draws
    // no in-between frames for them whatever the refresh rate setting.
    const bool replay = (pad_entry_use[0].source == PadSource::Playback) && (controller_polls - pad_entry_use[0].poll <= 4);
    const bool intro = read_word(rdram, mode_func_ptr) == intro_mode;
    static int last = -1;
    static const bool rate_log = std::getenv("GT_REPLAY_LOG") != nullptr;
    if (rate_log && int(replay || intro) != last) {
        last = int(replay || intro);
        std::printf("[replay] original frame rate only %d (replay %d intro %d)\n", last, int(replay), int(intro));
    }
    RT64::setOriginalRateOnly(replay || intro);
    static const bool log = std::getenv("GT_REPLAY_LOG") != nullptr;
    if (log && (controller_polls % 120) == 0) {
        const int16_t m0 = *reinterpret_cast<int16_t*>(rdram + ((0x800F1450u ^ 2) - 0x80000000u));
        std::printf("[replay] poll %u port0 mode %d buffer %08X count %d\n", controller_polls, m0,
            read_word(rdram, 0x800E8844), int32_t(read_word(rdram, 0x800E8848)));
    }
}

extern "C" void gt_pad_entry(uint8_t* rdram, int port, uint32_t entry_ptr_addr, int playback) {
    if (port < 0 || port >= 2) {
        return;
    }
    pad_entry_use[port] = { playback ? PadSource::Playback : PadSource::Recording, read_word(rdram, entry_ptr_addr), controller_polls };
}

extern "C" void gt_analog_pedals(uint8_t* rdram, uint32_t control, uint32_t pad) {
    // pad = &D_800F1420[port] (OSContPad: u16 buttons, s8 x, s8 y, u8 errno, 6 bytes); control = car + 0x200.
    constexpr uint32_t pads = 0x800F1420;
    if (pad < pads || ((pad - pads) % 6) != 0 || (pad - pads) / 6 >= 4) {
        return;
    }
    const int port = int((pad - pads) / 6);
    PedalCap& cap = pedal_caps[port];
    cap = PedalCap{};
    cap.car = control - 0x200;

    // The buffer entry the controller thread recorded or played back for this port in its latest polls.
    PadSource source = PadSource::Live;
    uint32_t entry = 0;
    if (port < 2 && pad_entry_use[port].entry != 0 && controller_polls - pad_entry_use[port].poll <= 2) {
        source = pad_entry_use[port].source;
        entry = pad_entry_use[port].entry;
    }
    cap.live = source != PadSource::Playback;
    if (source == PadSource::Playback) {
        // The pedals recorded with this entry; the entry itself or, if the game read past it, the latest one before.
        auto it = recorded_pedals.upper_bound(entry);
        if (it != recorded_pedals.begin()) {
            --it;
            if (entry - it->first <= 16 && it->second.contents == read_word(rdram, it->first)) {
                cap.throttle = it->second.throttle;
                cap.brake = it->second.brake;
            }
        }
        static const bool log = std::getenv("GT_REPLAY_LOG") != nullptr;
        if (log) {
            static int n = 0;
            if ((n++ % 60) == 0) {
                std::printf("[replay] port %d play entry %08X throttle %.2f brake %.2f\n", port, entry, cap.throttle, cap.brake);
            }
        }
        return;
    }

    float throttle_cap = 1.0f, brake_cap = 1.0f;
    if (gt::get_analog_triggers() == gt::AnalogTriggers::On) {
        const uint16_t buttons = *reinterpret_cast<uint16_t*>(rdram + ((pad ^ 2) - 0x80000000u));
        float throttle = n64_button_analog(port, recompinput::GameInput::A);
        float brake = n64_button_analog(port, recompinput::GameInput::B);
        // Testing: GT_PEDAL_TEST=<amount> stands in for the trigger while A is held (GT_INPUT).
        static const char* test = std::getenv("GT_PEDAL_TEST");
        if (test != nullptr && port == 0) {
            throttle = (buttons & 0x8000) ? float(std::atof(test)) : 0.0f;
        }
        // Only cap when the game's A/B bits match the live inputs (the attract-mode demo feeds recorded inputs).
        const bool live_a = throttle >= recompinput::axis_digital_threshold || test != nullptr;
        const bool live_b = brake >= recompinput::axis_digital_threshold;
        if (live_a == ((buttons & 0x8000) != 0)) {
            throttle_cap = pedal_amount(throttle);
        }
        if (live_b == ((buttons & 0x4000) != 0)) {
            brake_cap = pedal_amount(brake);
        }
    }
    cap.throttle = throttle_cap;
    cap.brake = brake_cap;
    if (source == PadSource::Recording) {
        recorded_pedals[entry] = { read_word(rdram, entry), throttle_cap, brake_cap };
    }
    static const bool log = std::getenv("GT_REPLAY_LOG") != nullptr;
    if (log) {
        static int n = 0;
        if ((n++ % 60) == 0) {
            std::printf("[replay] port %d %s entry %08X throttle %.2f brake %.2f (%zu kept)\n", port,
                source == PadSource::Recording ? "rec" : "live", entry, throttle_cap, brake_cap, recorded_pedals.size());
        }
    }
}

extern "C" void gt_pedal_begin(uint32_t car) {
    pedal_car = car;
}

void gt_traction_control(uint8_t* rdram, uint32_t car, bool brake);

extern "C" void gt_pedal_end(uint8_t* rdram, int brake) {
    for (const PedalCap& cap : pedal_caps) {
        if (cap.car != 0 && cap.car == pedal_car) {
            cap_pedal(rdram, pedal_car + (brake ? 0x188 : 0x180), brake ? cap.brake : cap.throttle);
            gt_traction_control(rdram, pedal_car, brake != 0);
            static const bool log = std::getenv("GT_PEDAL_LOG") != nullptr;
            if (log && !brake) {
                static int n = 0;
                if ((n++ % 30) == 0) {
                    uint32_t hi = *reinterpret_cast<uint32_t*>(rdram + (pedal_car + 0x180 - 0x80000000u));
                    uint32_t lo = *reinterpret_cast<uint32_t*>(rdram + (pedal_car + 0x184 - 0x80000000u));
                    uint64_t bits = (uint64_t(hi) << 32) | lo; double v; memcpy(&v, &bits, 8);
                    uint32_t shi = *reinterpret_cast<uint32_t*>(rdram + (pedal_car + 0x158 - 0x80000000u));
                    uint32_t slo = *reinterpret_cast<uint32_t*>(rdram + (pedal_car + 0x15C - 0x80000000u));
                    uint64_t sbits = (uint64_t(shi) << 32) | slo; double sp; memcpy(&sp, &sbits, 8);
                    fprintf(stderr, "[pedals] car %08X throttle cap %.2f pedal %.3f speed %.2f\n", pedal_car, cap.throttle, v, sp);
                }
            }
        }
    }
}

// Draw distance. The race view draws the track as a ring of sections (D_80114588 + 1 of them at *D_80114584, 0x28
// bytes each) in func_8000A350: the camera's current one and a fixed number behind and ahead (camera +0xFE / +0xFC, or
// +0x100 facing the other way), about 7 and 11. Each section's display list then picks a detailed model, a simpler one
// or nothing by the depth of a reference vertex (G_BRANCH_Z at 1020 and 1022, about 5700 units), which RT64 now always
// takes while the far plane is extended (RT64::setFarScale). Drawing more sections needs:
// - more list nodes than the game's pool (128 nodes of 0x28 at D_8011A638..D_8011BA38): the rest go to the Expansion
//   Pak's 4 MB, which GT64 doesn't use;
// - more matrices than its buffers (0x6280 bytes each at D_8011EA48) and more display list than its (0xA000 each at
//   D_800FBFC8; the intro's split screen draws two race views a frame, and past the buffer is its own write pointer,
//   D_8010FFC8): both moved there too;
// - the sections' geometry: it's streamed (see gt_track_segment).
namespace {
    constexpr uint32_t ext_nodes = 0x80400000u;     // list nodes past the game's pool (0x10000)
    // Two display list buffers of 0x18000 and two matrix buffers of 0x20000. A frame takes about 0x7C and 0x170 bytes
    // per track section (EUR GP Short's whole ring of 165: 0x5040 and 0xE900), and Maximum draws only the sections
    // within max_draw_offset. 0x80480000 on: HD wheels (src/game/hd_wheels.cpp).
    constexpr uint32_t ext_gfx = 0x80410000u;
    constexpr uint32_t ext_gfx_size = 0x18000u;
    constexpr uint32_t ext_matrices = 0x80440000u;
    constexpr uint32_t ext_matrix_size = 0x20000u;
    constexpr double max_draw_offset = 32000.0; // largest camera-relative position an N64 matrix holds (32767), less a margin
    constexpr uint32_t ext_track = 0x80500000u;     // the track's geometry block, copied from ROM
    constexpr uint32_t ext_track_size = 0x300000u;
    constexpr uint32_t chunk_table = 0x80160950u;
    uint32_t ext_track_rom = 0;

    bool extended_draw_distance() {
        return gt::get_draw_distance() != gt::DrawDistance::Original;
    }

    uint32_t rd32(uint8_t* rdram, uint32_t a) { return *reinterpret_cast<uint32_t*>(rdram + (a - 0x80000000u)); }
    void wr32(uint8_t* rdram, uint32_t a, uint32_t v) { *reinterpret_cast<uint32_t*>(rdram + (a - 0x80000000u)) = v; }
    void wr16(uint8_t* rdram, uint32_t a, uint16_t v) { *reinterpret_cast<uint16_t*>(rdram + ((a ^ 2) - 0x80000000u)) = v; }
    uint16_t rd16(uint8_t* rdram, uint32_t a) { return *reinterpret_cast<uint16_t*>(rdram + ((a ^ 2) - 0x80000000u)); }
    double rd64f(uint8_t* rdram, uint32_t a) {
        const uint64_t bits = (uint64_t(rd32(rdram, a)) << 32) | rd32(rdram, a + 4);
        double d;
        std::memcpy(&d, &bits, sizeof(d));
        return d;
    }
    void wr64f(uint8_t* rdram, uint32_t a, double d) {
        uint64_t bits;
        std::memcpy(&bits, &d, sizeof(d));
        wr32(rdram, a, uint32_t(bits >> 32));
        wr32(rdram, a + 4, uint32_t(bits));
    }
    float rd32f(uint8_t* rdram, uint32_t a) {
        const uint32_t bits = rd32(rdram, a);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }
    void wr32f(uint8_t* rdram, uint32_t a, float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(f));
        wr32(rdram, a, bits);
    }

    // Race views drawn this frame and last (func_8000A350 runs once per view; the intro's split screen draws two).
    uint32_t views_this_frame = 0;
    uint32_t game_frame = 0;   // frames built (gt_matrix_buffer)
    uint32_t views_last_frame = 0;
    // The frame (game_frame) each track section was last drawn on, by its index in the ring.
    std::vector<uint32_t> section_drawn;

    int32_t section_ring(uint8_t* rdram) {
        return int32_t(rd16(rdram, 0x80114588u)) + 1;
    }

    // How far `sections` track sections reach: their average spacing on this track (measured once per track) times
    // the count, plus a margin.
    double object_reach(uint8_t* rdram, int32_t sections) {
        static uint32_t measured_base = 0;
        static double spacing = 0.0;
        const uint32_t base = rd32(rdram, 0x80114584u);
        const int32_t ring = section_ring(rdram);
        if (base != measured_base && base >= 0x80000000u && base < 0x80800000u && ring > 1) {
            double total = 0.0;
            for (int32_t i = 0; i < ring; i++) {
                const uint32_t a = base + i * 0x28, b = base + ((i + 1) % ring) * 0x28;
                const double dx = rd64f(rdram, b + 0x08) - rd64f(rdram, a + 0x08);
                const double dy = rd64f(rdram, b + 0x10) - rd64f(rdram, a + 0x10);
                const double dz = rd64f(rdram, b + 0x18) - rd64f(rdram, a + 0x18);
                total += std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            spacing = total / ring;
            measured_base = base;
            if (std::getenv("GT_CULL_LOG")) {
                std::printf("[cull] %d sections, %.0f units apart: objects within %.0f\n", ring, spacing, std::max(7000.0, spacing * sections * 1.1));
            }
        }
        return std::max(7000.0, spacing * sections * 1.1);
    }

    // Sections either way that stay within max_draw_offset of the camera: by the track's average spacing, less a margin
    // for the camera (not at its section's origin) and longer steps. gt_add_section drops the rare one still past it.
    int32_t sections_in_range(uint8_t* rdram) {
        const double spacing = section_spacing(rdram);
        return (spacing > 0.0) ? int32_t((max_draw_offset - 2000.0) / (spacing * 1.1)) : 20;
    }

    // The track's average section spacing (0 before it's been measured).
    double section_spacing(uint8_t* rdram) {
        object_reach(rdram, 1);
        const double reach = object_reach(rdram, 1000);
        return reach / (1000 * 1.1);
    }

    int32_t original_behind = 0; // sections the game itself draws behind (gt_sections_behind's count)
    // Track sections added and dropped (too far from the camera) on the frame being built, and its views' start section.
    uint32_t frame_sections_added = 0, frame_sections_dropped = 0;
    int32_t frame_start_section = -1;
    // Its reach in the camera's direction facing forward (11 ahead, 1 behind); facing backward it draws 7 behind and 1
    // ahead.
    int32_t original_forward = 0;

    // Sections drawn behind and ahead of the camera's: Extended 10 and 20, Maximum as many as fit (Extended's counts while
    // the game draws more than one view a frame: the whole track several times over is too slow).
    void section_counts(uint8_t* rdram, int32_t& behind, int32_t& ahead) {
        const int32_t ring = section_ring(rdram);
        const uint32_t views = std::max(views_this_frame, views_last_frame);
        const bool all = (gt::get_draw_distance() == gt::DrawDistance::Maximum) && (views <= 1);
        // Maximum: as many as stay within max_draw_offset (see gt_add_section); the whole track on short ones.
        const int32_t most = all ? std::max(20, sections_in_range(rdram)) : 20;
        behind = std::min(all ? most : 10, (ring - 1) / 2);
        ahead = std::min(most, ring - 1 - behind);
    }
}

// The section a race view draws the track from (its +0xF8, copied from the camera's each frame) is the ring section
// nearest the camera, found by a search around the last one. Off the track it has been seen to end up on a section far
// from the camera, and the whole track was then drawn away from it (nothing in sight but the cars and sky). When the
// view's section is well over twice as far from the camera as the nearest one, the view starts from the nearest instead
// (logged). Only this frame's drawing changes: the game's own state is untouched.
extern "C" void gt_view_start(uint8_t* rdram, uint32_t view) {
    const uint32_t base = rd32(rdram, 0x80114584u);
    const int32_t ring = section_ring(rdram);
    if (base < 0x80000000u || base >= 0x80800000u || ring < 2 || ring > 0x1000) {
        return;
    }
    const int32_t start = int16_t(rd16(rdram, view + 0xF8));
    const double cx = rd64f(rdram, view + 0xD8), cy = rd64f(rdram, view + 0xE0), cz = rd64f(rdram, view + 0xE8);
    auto dist2 = [&](int32_t i) {
        const uint32_t a = base + uint32_t(i) * 0x28;
        const double dx = rd64f(rdram, a + 0x08) - cx, dy = rd64f(rdram, a + 0x10) - cy, dz = rd64f(rdram, a + 0x18) - cz;
        return dx * dx + dy * dy + dz * dz;
    };
    int32_t nearest = 0;
    double nearest_d2 = dist2(0);
    for (int32_t i = 1; i < ring; i++) {
        const double d2 = dist2(i);
        if (d2 < nearest_d2) {
            nearest_d2 = d2;
            nearest = i;
        }
    }
    const double start_d = (start >= 0 && start < ring) ? std::sqrt(dist2(start)) : 1e30;
    const double nearest_d = std::sqrt(nearest_d2);
    const double spacing = section_spacing(rdram);
    const double slack = std::max(3000.0, 3.0 * spacing);
    if (start_d > 2.0 * nearest_d + slack) {
        static uint32_t logged = 0;
        if (logged++ < 20) {
            std::fprintf(stderr, "[track] f%u view section %d is %.0f from the camera, nearest %d at %.0f: drawn from %d\n", game_frame,
                start, start_d, nearest, nearest_d, nearest);
        }
        wr16(rdram, view + 0xF8, uint16_t(nearest));
    }
}

extern "C" int32_t gt_sections_behind(uint8_t* rdram, int32_t count) {
    original_behind = count;
    if (ovl_front_loaded) {
        ext_gfx_stale_race[building_buffer] = true;
    }
    frame_start_section = int16_t(rd16(rdram, rd32(rdram, 0x8010FFCCu) + 0xF8));
    views_this_frame++;
    track_views++;
    // Objects outside the track ring (the cars, their wheels and shadows, and trackside scenery: each frame's object
    // list, func_80009698) are culled by angle and squared distance from the camera: always within 1000 units, out to
    // 7000 inside a view cone meant for 4:3, 200 behind. Extended draws them within the reach of the track it draws (so
    // no cars beyond its end), Maximum all of them.
    const bool extended = extended_draw_distance();
    if (!extended) {
        wr64f(rdram, 0x800AA608u, 1e6);
        wr64f(rdram, 0x800AA610u, 4.9e7);
        wr64f(rdram, 0x800AA618u, 4e4);
        return count;
    }
    int32_t behind, ahead;
    section_counts(rdram, behind, ahead);
    // (Not past where a section's or object's matrix can hold its position: see gt_add_section.)
    // Within the view cone only (widened for widescreen, gt_cone_half_angle): objects behind the camera, past the
    // game's own 1000 all around and 200 behind, aren't sent to be drawn (the renderer showed them upside down in the
    // sky ahead).
    const double reach = std::min((ahead >= section_ring(rdram) / 2) ? 1e6 : object_reach(rdram, ahead), max_draw_offset);
    wr64f(rdram, 0x800AA608u, 1e6);
    wr64f(rdram, 0x800AA610u, reach * reach);
    wr64f(rdram, 0x800AA618u, 4e4);
    return std::max(count, behind);
}

// Half the object list's view cone in degrees (func_80009698): the game's 45 is wide enough for 4:3; with an extended
// draw distance, 65 covers up to 21:9 and the width of a car at the edge.
// The AI update's mode (func_80032874, called from func_80028604 for each AI car): see the hook in gt64.us.toml.
static bool ai_update = false;

extern "C" void gt_ai_update_begin(void) {
    ai_update = true;
}

extern "C" uint32_t gt_ai_update_mode(uint32_t mode) {
    if (ai_update && extended_draw_distance()) {
        mode |= 5u;
    }
    return mode;
}

extern "C" float gt_cone_half_angle(void) {
    return extended_draw_distance() ? 65.0f : 45.0f;
}

extern "C" int32_t gt_sections_ahead(uint8_t* rdram, int32_t count) {
    if (count >= original_behind) {
        original_forward = count;
    } else if (original_forward == 0) {
        original_forward = original_behind;
    }
    // Motion blur: the camera's position (*D_8010FFCC + 0xD8, doubles; the track is drawn relative to it, the view matrix
    // only turns), for RT64 to tell how far the scene moved since the last frame. Not in split screen.
    {
        const uint32_t cam = rd32(rdram, 0x8010FFCCu);
        // Only above the game's own 30 FPS (Framerate not Original): at 30 FPS the blur reads as smearing.
        const bool valid = (gt::get_motion_blur() == gt::MotionBlur::On) &&
            (ultramodern::renderer::get_graphics_config().rr_option != ultramodern::renderer::RefreshRate::Original) &&
            (views_this_frame <= 1) && (views_last_frame <= 1) &&
            (cam >= 0x80000000u) && (cam < 0x80800000u);
        RT64::setMotionCamera(valid ? rd64f(rdram, cam + 0xD8) : 0.0, valid ? rd64f(rdram, cam + 0xE0) : 0.0, valid ? rd64f(rdram, cam + 0xE8) : 0.0, valid);
    }
    if (!extended_draw_distance()) {
        return count;
    }
    int32_t behind, ahead;
    section_counts(rdram, behind, ahead);
    return std::max(count, ahead);
}

// func_8000A280, rewritten: append a list node for track section `section` after the pool's last node; past the game's
// pool, nodes go to the Expansion Pak.
extern "C" void gt_add_section(uint8_t* rdram, uint32_t section) {
    constexpr uint32_t pool_ptr = 0x8011BA38u;  // also the pool's end
    // Past 32767 units from the camera (any axis), a section's position doesn't fit the N64's fixed-point matrix
    // (s15.16) and wraps around: Maximum's whole track then drew far pieces somewhere near the camera, turned their own
    // way (a barrier appearing beside the road for a frame). Those aren't drawn.
    {
        const uint32_t cam = rd32(rdram, 0x8010FFCCu);
        const double dx = rd64f(rdram, section + 0x08) - rd64f(rdram, cam + 0xD8);
        const double dy = rd64f(rdram, section + 0x10) - rd64f(rdram, cam + 0xE0);
        const double dz = rd64f(rdram, section + 0x18) - rd64f(rdram, cam + 0xE8);
        if (std::max({ std::fabs(dx), std::fabs(dy), std::fabs(dz) }) > max_draw_offset) {
            frame_sections_dropped++;
            return;
        }
        frame_sections_added++;
    }
    {
        const uint32_t base = rd32(rdram, 0x80114584u);
        const uint32_t index = (section - base) / 0x28;
        if (section >= base && index < 0x1000u) {
            if (section_drawn.size() <= index) {
                section_drawn.resize(index + 1, 0);
            }
            section_drawn[index] = game_frame + 1;
        }
    }
    const uint32_t tail = rd32(rdram, pool_ptr);
    uint32_t node = tail + 0x28;
    if (tail < ext_nodes && node + 0x28 > pool_ptr) {
        node = ext_nodes;
    }
    wr32(rdram, pool_ptr, node);
    wr32(rdram, node + 0x10, rd32(rdram, tail + 0x10));
    wr32(rdram, tail + 0x10, node);
    wr16(rdram, node + 0x0, rd16(rdram, section + 0x24));
    wr16(rdram, node + 0x2, rd16(rdram, section + 0x26));
    wr32(rdram, node + 0x14, rd32(rdram, section + 0x0));
    const uint32_t cam = rd32(rdram, 0x8010FFCCu);
    wr32f(rdram, node + 0x18, float(rd64f(rdram, section + 0x08) - rd64f(rdram, cam + 0xD8)));
    wr32f(rdram, node + 0x1C, float(rd64f(rdram, section + 0x10) - rd64f(rdram, cam + 0xE0)));
    wr32f(rdram, node + 0x20, float(rd64f(rdram, section + 0x18) - rd64f(rdram, cam + 0xE8)));
}

// A wheel just added to the frame's object list (func_8000BB70) for the current car (*D_800DB5D4). The game only
// updates a car's wheels while it draws them (cars about 2 track sections or more from the camera are drawn as bodies
// alone), so on a car's first frame back in full detail they're where they were when it went: that frame, drop them.
extern "C" void gt_wheel_drawn(uint8_t* rdram) {
    struct Seen {
        uint32_t frame = 0;
        bool fresh = false;
    };
    static std::map<uint32_t, Seen> seen;
    const uint32_t car = rd32(rdram, 0x800DB5D4u);
    Seen& s = seen[car];
    if (s.frame != game_frame) {
        s.fresh = (s.frame + 1 != game_frame);
        s.frame = game_frame;
    }
    if (!s.fresh) {
        return;
    }
    const uint8_t index = rdram[(0x8011A594u ^ 3) - 0x80000000u];
    const uint32_t list = 0x80110928u + uint32_t(index) * 0x1E08u;
    const uint16_t count = rd16(rdram, list);
    if (count > 0) {
        wr16(rdram, list, uint16_t(count - 1));
    }
}

// Cars drawn as bodies alone (+0x119 bit 0x10, 2 to about 5 track sections from the camera) in full instead, see
// gt64.us.toml. The current car is *D_800DB5D0.
extern "C" void gt_car_detail(uint8_t* rdram) {
    ai_update = false;
    const uint32_t car = rd32(rdram, 0x800DB5D0u);
    if (car < 0x80000000u || car >= 0x80800000u) {
        return;
    }
    uint8_t& flags = rdram[((car + 0x119) ^ 3) - 0x80000000u];
    if (!(flags & 0x20)) {
        flags &= uint8_t(~0x10);
    }
    // Further out (0x20) the game doesn't draw the car at all, which hid it in the fog of its own draw distance; with an
    // extended one, cars around that point came and went every few frames. Draw them in full (body-only, without the
    // wheels that fill the gap under it, a car seemed to hover), but only on a track section drawn this frame or the
    // last (the car's is +0x16A): one past the end of the track drawn, or on a stretch of it out of sight, seemed to fly
    // over nothing.
    else if (extended_draw_distance()) {
        const uint32_t section = rd16(rdram, car + 0x16A);
        if (section < section_drawn.size() && section_drawn[section] != 0 && section_drawn[section] >= game_frame) {
            flags = uint8_t(flags & ~0x30);
        }
    }
}

// Matrix groups for the car parts (see gt64.us.toml): RT64's extended GBI, enabled with a no-op hook (F3DEX: opcode
// 0x00 with RT64's magic number) and then commands with opcode 0x64. A part's ID is its car (the object's +2) and its
// name (+5: "BODY", "FL TIRE", "SHADOW"...), the same across the body's detail levels.
namespace {
    bool part_group_open = false;
    void gfx_put(uint8_t* rdram, uint32_t w0, uint32_t w1) {
        const uint32_t gfx = rd32(rdram, 0x8010FFC8u);
        wr32(rdram, gfx, w0);
        wr32(rdram, gfx + 4, w1);
        wr32(rdram, 0x8010FFC8u, gfx + 8);
    }
}

namespace { uint32_t part_obj = 0; }

extern "C" void gt_part_begin(uint8_t* rdram, uint32_t node) {
    part_group_open = false;
    const uint32_t obj = rd32(rdram, node + 0x14);
    if (obj < 0x80000000u || obj >= 0x80800000u) {
        return;
    }
    const uint8_t flag = rdram[((obj + 4) ^ 3) - 0x80000000u];
    if (flag != 0x60 && flag != 0x20) {
        return;   // not a car's body, wheel or shadow
    }
    uint32_t id = 2166136261u;
    auto mix = [&](uint8_t b) { id = (id ^ b) * 16777619u; };
    const uint16_t car = rd16(rdram, obj + 2);
    mix(uint8_t(car)); mix(uint8_t(car >> 8));
    char name[12] = {};
    for (uint32_t i = 5; i < 16; i++) {
        const uint8_t c = rdram[((obj + i) ^ 3) - 0x80000000u];
        if (c == 0) break;
        mix(c);
        name[i - 5] = char(c);
    }
    // A wheel turns a long way between frames (spinning, and more so in a wheelspin or slide): interpolating its matrix
    // component by component shrank it on the frames drawn in between (the in-between of two rotations is a smaller
    // matrix), so the wheels pulsed in size. Wheels are decomposed instead: rotation interpolated as a rotation, scale
    // on its own.
    const bool wheel = std::strstr(name, "TIRE") != nullptr;
    id |= 0x80000000u;   // never G_EX_ID_IGNORE (0) or G_EX_ID_AUTO (0xFFFFFFFF)
    if (id == 0xFFFFFFFFu) id = 0x80000001u;
    gfx_put(rdram, 0x00525464u, (0x1u << 28) | 0x64u);       // enable the extended GBI at opcode 0x64
    gfx_put(rdram, 0x6400000Cu, id);                          // gEXMatrixGroupSimple(id, push, modelview, position and
    // rotation interpolated, the rest skipped, linear order); wheels gEXMatrixGroupDecomposed with scale interpolated too
    gfx_put(rdram, wheel ? 0x000000ADu : 0x00000029u, 0);
    part_group_open = true;
    part_obj = obj;
}

extern "C" void gt_part_end(uint8_t* rdram) {
    if (part_group_open) {
        static const bool log = std::getenv("GT_PARTLOG") != nullptr;
        if (log) {
            // The part's last modelview load, back from here: its translation (camera space) and the object's position.
            const uint32_t end = rd32(rdram, 0x8010FFC8u);
            for (uint32_t a = end - 8; a + 0x200 > end; a -= 8) {
                if ((rd32(rdram, a) >> 24) == 0x01) {
                    const uint32_t m = 0x80000000u | (rd32(rdram, a + 4) & 0x7FFFFFu);
                    auto fx = [&](int i) {
                        const uint32_t hi = rd32(rdram, m + (i / 2) * 4), lo = rd32(rdram, m + 0x20 + (i / 2) * 4);
                        const int16_t ih = int16_t((i & 1) ? (hi & 0xFFFF) : (hi >> 16));
                        const uint16_t fl = uint16_t((i & 1) ? (lo & 0xFFFF) : (lo >> 16));
                        return ih + fl / 65536.0;
                    };
                    const uint32_t cam = rd32(rdram, 0x8010FFCCu);
                    auto rd_double = [](uint8_t* r, uint32_t x) {
                        const uint64_t bits = (uint64_t(rd32(r, x)) << 32) | rd32(r, x + 4);
                        double v; std::memcpy(&v, &bits, sizeof(v)); return v;
                    };
                    char nm[12] = {};
                    for (int i = 0; i < 11; i++) nm[i] = char(rdram[((part_obj + 5 + i) ^ 3) - 0x80000000u]);
                    std::fprintf(stderr, "[part] f%u car%u %s P %.1f %.1f %.1f C %.1f %.1f %.1f M", game_frame, rd16(rdram, part_obj + 2), nm,
                        rd_double(rdram, part_obj + 0x18), rd_double(rdram, part_obj + 0x20), rd_double(rdram, part_obj + 0x28),
                        rd_double(rdram, cam + 0xD8), rd_double(rdram, cam + 0xE0), rd_double(rdram, cam + 0xE8));
                    for (int i = 0; i < 16; i++) std::fprintf(stderr, " %.3f", fx(i));
                    std::fprintf(stderr, "\n");
                    break;
                }
            }
        }
        gfx_put(rdram, 0x6400000Du, 1);                       // gEXPopMatrixGroup(modelview)
        part_group_open = false;
    }
}

// The frame's display list and matrix buffers (D_8010FFC8 and D_8012AF48, just set from buffer index `index`, nothing
// written yet): in the Expansion Pak.
namespace {
    uint32_t ext_gfx_built[2] = {};                     // the frame (game_frame) each buffer was last built on
    uint32_t ext_gfx_loads[2] = {};                     // front_overlay_loads when each buffer was last built
    std::unordered_map<uint32_t, uint32_t> ext_task_build; // by a task's display list address: the build it was submitted from
}

extern "C" void gt_matrix_buffer(uint8_t* rdram, uint32_t index) {
    // A race view last frame that drew no track at all (the game's own sections all too far from the camera): logged,
    // a few times a race, with where the camera was and the section the game started from.
    if (views_this_frame > 0 && frame_sections_added == 0) {
        static uint32_t logged = 0, logged_ring = 0;
        const uint32_t ring_base = rd32(rdram, 0x80114584u);
        if (ring_base != logged_ring) { logged_ring = ring_base; logged = 0; }
        if (logged++ < 10) {
            const uint32_t cam = rd32(rdram, 0x8010FFCCu);
            std::fprintf(stderr, "[track] f%u no track drawn: start section %d of %d, %u dropped, camera %.0f %.0f %.0f\n", game_frame,
                frame_start_section, section_ring(rdram), frame_sections_dropped, rd64f(rdram, cam + 0xD8), rd64f(rdram, cam + 0xE0), rd64f(rdram, cam + 0xE8));
        }
    }
    frame_sections_added = 0;
    frame_sections_dropped = 0;
    game_frame++;
    ext_gfx_built[index & 1] = game_frame;
    ext_gfx_loads[index & 1] = front_overlay_loads;
    building_buffer = index & 1;
    ext_gfx_stale_race[index & 1] = false;
    views_last_frame = views_this_frame;
    views_this_frame = 0;
    if (extended_draw_distance()) {
        wr32(rdram, 0x8010FFC8u, ext_gfx + (index & 1) * ext_gfx_size);
        wr32(rdram, 0x8012AF48u, ext_matrices + (index & 1) * ext_matrix_size);
    }
}

// A graphics task submitted (func_80007C80) again from one of the Expansion Pak display list buffers without the buffer
// having been built again: leaving a race, GT64 submits its last race frame once more after the next screen's data has
// been loaded over the cars' and track's (on the N64 its own buffer has been rewritten by then). Its display list then
// ran into that data, and the renderer crashed or stopped drawing. It's emptied instead (G_ENDDL).
extern "C" void gt_gfx_task_check(uint8_t* rdram, uint32_t task) {
    if (rd32(rdram, task) != 1) return;  // M_GFXTASK
    const uint32_t dl = rd32(rdram, task + 0x30) | 0x80000000u;
    if (dl < ext_gfx || dl >= ext_gfx + 2 * ext_gfx_size) return;
    const uint32_t buffer = (dl - ext_gfx) / ext_gfx_size;
    const uint32_t build = ext_gfx_built[buffer];
    auto it = ext_task_build.find(dl);
    // Also any task from a buffer built before the front-end overlay was last read in: leaving a race, the game submits
    // the frame it was building (its HUD list still the last frame's, calling into race data the next screen's has
    // since replaced) after loading it.
    // And any from a buffer that drew a race view after the front-end overlay was read in: the game can still build a
    // race frame or two then, from race data the overlay and the next screen's data are replacing.
    if ((it != ext_task_build.end() && it->second == build) || (ext_gfx_loads[buffer] != front_overlay_loads) ||
        ext_gfx_stale_race[buffer]) {
        static const bool log = std::getenv("GT_TASKLOG") != nullptr;
        if (log) std::fprintf(stderr, "[gfxtask] task 0x%08X stale (built on f%u): emptied\n", dl, build);
        wr32(rdram, dl, 0xB8000000u);
        wr32(rdram, dl + 4, 0);
        return;
    }
    ext_task_build[dl] = build;
}

// Track geometry is streamed: each section's chunk (segment 0x0B, a slice of one contiguous block of ROM) is DMAed into a
// cache slot only while it's near the camera, and its entry in the chunk table (D_80160950, 20 bytes: RAM address or -1,
// ROM address, size, offset in the segment, slot size) says where; func_8000A570 sets segment 0x0B to RAM address -
// offset for each node. Past the game's own range a chunk is either not loaded or its entry still names a slot that has
// since been refilled with another chunk, so with the extended draw distance every node draws from a copy of the whole
// block in the Expansion Pak, read once per track (it's the same data the game loads) and checked against the ROM a slice each frame.
extern "C" void gt_track_segment(uint8_t* rdram, recomp_context* ctx) {
    if (!extended_draw_distance()) {
        return;
    }
    // The block's ROM address: from this node's own chunk entry (v1) when it's filled in (its ROM address less its offset
    // in the block). At the start of a race the entries of chunks not loaded yet are still blank (RAM -1, ROM 0), and
    // the game's own segment for them, -1, ran the track's display list into code (RT64 then stopped every task: no
    // track, then a crash). The node's list addresses are offsets in the block, so it draws from the copy all the same:
    // one taken from any chunk that is loaded, or the copy already taken.
    const auto rom = recomp::get_rom();
    const uint32_t entry = uint32_t(ctx->r3);
    uint32_t rom_base = 0;
    if (entry >= chunk_table && entry < chunk_table + 512 * 20 && rd32(rdram, entry + 4) != 0) {
        rom_base = rd32(rdram, entry + 4) - rd32(rdram, entry + 12);
    }
    for (uint32_t i = 0; (rom_base == 0 || rom_base >= rom.size()) && (i < 512); i++) {
        const uint32_t e = chunk_table + i * 20;
        if (rd32(rdram, e) != 0xFFFFFFFFu && rd32(rdram, e + 4) != 0) {
            rom_base = rd32(rdram, e + 4) - rd32(rdram, e + 12);
        }
    }
    if (rom_base == 0 || rom_base >= rom.size()) {
        rom_base = ext_track_rom;
    }
    if (rom_base == 0 || rom_base >= rom.size()) {
        return;
    }
    const size_t size = std::min<size_t>(ext_track_size, rom.size() - rom_base);
    auto copy = [&](size_t from, size_t to) {
        for (size_t i = from; i < to; i++) {
            rdram[((ext_track + i) ^ 3) - 0x80000000u] = rom[rom_base + i];
        }
    };
    static uint32_t verified_frame = 0;
    static size_t verify_at = 0;
    if (rom_base != ext_track_rom) {
        copy(0, size);
        std::fprintf(stderr, "[track] geometry copied from ROM 0x%08X (0x%zX bytes)%s\n", rom_base, size,
            (ext_track_rom != 0) ? ", replacing another track's" : "");
        ext_track_rom = rom_base;
        verify_at = 0;
    } else if (verified_frame != game_frame) {
        // Once a frame, check the next 128 KB of the copy against the ROM (all of it about every 1 s): anything that
        // overwrote it would leave the track drawing garbage or nothing for the rest of the session.
        verified_frame = game_frame;
        const size_t end = std::min(size, verify_at + 0x20000);
        for (size_t i = verify_at; i < end; i++) {
            if (rdram[((ext_track + i) ^ 3) - 0x80000000u] != rom[rom_base + i]) {
                std::fprintf(stderr, "[track] geometry copy damaged at +0x%zX: copied again\n", i);
                copy(0, size);
                break;
            }
        }
        verify_at = (end >= size) ? 0 : end;
    }
    ctx->r8 = int32_t(ext_track);
    ctx->r9 = 0;
}

// The save thread's requests (func_80005B80: 10-18 are the Controller Pak calls, 18 motor init).
extern "C" void gt_save_request(uint8_t* rdram, uint32_t request) {
    static const bool log = std::getenv("GT_PAK_LOG") != nullptr;
    if (log) {
        std::fprintf(stderr, "[save] request %d (mode %08X)\n", int(int16_t(rd16(rdram, request))), rd32(rdram, 0x800E8810u));
    }
}


// Automatic gearbox (General > Automatic Gearbox: Original or RobotBox, the Custom setting).
// Each car's gearbox runs in func_8003412C: with an automatic transmission it calls func_800338B4, which takes the car
// out of neutral, handles reverse, and shifts up when the engine speed (double at car +0x100) reaches the car's upshift
// point and down (from 2nd or higher, with a pedal pressed) when it falls to its downshift point. The points are a
// table of float pairs at 0x80093158 indexed by car +0x2D0; the rev limit is 10000 RPM for every car. Custom swaps in
// 9000 and 5500 RPM for that call, for cars driven from a controller (live or a replay's recording, so the replay
// shifts the same), and also shifts down below 5500 with no pedal pressed. The gear is car +0x178: 0 reverse,
// 1 neutral, 2 first.
extern "C" void func_80034048(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr uint32_t shift_points = 0x80093158;
    constexpr float custom_up = 9000.0f;
    constexpr float custom_down = 5500.0f;
    uint32_t gearbox_car = 0;
    uint32_t gearbox_entry = 0;
    uint32_t saved_points[2];

    bool controller_car(uint32_t car) {
        for (const PedalCap& cap : pedal_caps) {
            if (cap.car == car) {
                return true;
            }
        }
        return false;
    }

    double read_double(uint8_t* rdram, uint32_t a) {
        const uint64_t bits = (uint64_t(read_word(rdram, a)) << 32) | read_word(rdram, a + 4);
        double v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

    int16_t read_half(uint8_t* rdram, uint32_t a) {
        return int16_t(*reinterpret_cast<uint16_t*>(rdram + ((a ^ 2) - 0x80000000u)));
    }

    void write_float(uint8_t* rdram, uint32_t a, float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        *reinterpret_cast<uint32_t*>(rdram + (a - 0x80000000u)) = bits;
    }
}

// Before the automatic gearbox (func_800338B4) runs for `car`.
extern "C" void gt_auto_gearbox_begin(uint8_t* rdram, uint32_t car) {
    gearbox_car = 0;
    if (gt::get_auto_gearbox() != gt::AutoGearbox::Custom || !controller_car(car)) {
        return;
    }
    const uint32_t index = read_word(rdram, car + 0x2D0);
    if (index >= 64) {
        return;
    }
    gearbox_car = car;
    gearbox_entry = shift_points + index * 8;
    saved_points[0] = read_word(rdram, gearbox_entry);
    saved_points[1] = read_word(rdram, gearbox_entry + 4);
    write_float(rdram, gearbox_entry, custom_up);
    write_float(rdram, gearbox_entry + 4, custom_down);
}

// After it: put the car's shift points back, and shift down when coasting below the downshift point.
extern "C" void gt_auto_gearbox_end(uint8_t* rdram, recomp_context* ctx) {
    if (gearbox_car == 0) {
        return;
    }
    const uint32_t car = gearbox_car;
    gearbox_car = 0;
    *reinterpret_cast<uint32_t*>(rdram + (gearbox_entry - 0x80000000u)) = saved_points[0];
    *reinterpret_cast<uint32_t*>(rdram + (gearbox_entry + 4 - 0x80000000u)) = saved_points[1];

    const int gear = read_half(rdram, car + 0x178);
    static const bool coast_test = std::getenv("GT_GEAR_COAST_TEST") != nullptr;  // testing: as if no pedal is pressed
    const bool pedal = !coast_test && (read_half(rdram, car + 0x200) != 0 || read_half(rdram, car + 0x204) != 0);
    if (gear >= 3 && !pedal && read_double(rdram, car + 0x100) <= custom_down) {
        ctx->r4 = int64_t(int32_t(car));  // registers hold sign-extended 32-bit values
        func_80034048(rdram, ctx);
    }
    static const bool log = std::getenv("GT_GEAR_LOG") != nullptr;
    if (log) {
        static int last_gear = -1;
        if (gear != last_gear) {
            std::printf("[gear] car %08X gear %d at %.0f RPM\n", car, read_half(rdram, car + 0x178) - 1, read_double(rdram, car + 0x100));
            last_gear = gear;
        }
    }
}

// Slip rumble: the Rumble Pak buzzes while a controller's car spins its driven wheels or slides, harder the more it
// slips. The game rumbles only for bumps and hits (its wheel contact code, func_80039AA4, starts a short pattern when
// a wheel lands: over cobbles that repeats, 3 frames on and 3 off). Slip rumble pulses the motor the same way, on for
// 1 to 3 of every 6 frames as the slip grows, so at full slip it feels like the cobbles. Its pulses are added to the
// game's own: the motor runs while either asks for it.
namespace {
    constexpr uint32_t rumble_status = 0x8019A2E0;  // per port, 0x18 bytes: byte 0 high nibble set when a Rumble Pak is ready,
                                                     // low nibble set while the game holds rumble off (paused)
    struct SlipRumblePort {
        bool game_on = false;   // the game's motor state (osMotorStart/Stop)
        bool slip_on = false;   // slip rumble's
        bool applied = false;
        uint32_t last_vi = 0;   // VI of the latest slip update
        int phase = 0;          // frame within the 6-frame pulse
    };
    std::mutex slip_rumble_mutex;
    SlipRumblePort slip_rumble_ports[4];
    std::atomic<uint32_t> slip_rumble_vi{0};

    void apply_rumble(int port) {  // with slip_rumble_mutex held
        SlipRumblePort& p = slip_rumble_ports[port];
        const bool on = p.game_on || p.slip_on;
        if (on != p.applied) {
            p.applied = on;
            recompinput::set_rumble(port, on);
        }
    }

    float clamp01(float v) {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    // How much a car is slipping, 0 (gripping) to 1: wheelspin from how far the driven wheels' revs are from the
    // revs its road speed gives in this gear, sliding from the angle between its heading and where it is going.
    float car_slip(uint8_t* rdram, uint32_t car) {
        const double speed = std::fabs(read_double(rdram, car + 0x110));   // km/h
        const double road_rpm = read_double(rdram, car + 0x100);
        const double wheel_rpm = read_double(rdram, car + 0x168);
        const int gear = read_half(rdram, car + 0x178);                    // 0 reverse, 1 neutral, 2 first...
        float spin = 0.0f;
        if (gear != 1) {
            const double ratio = std::fabs(wheel_rpm - road_rpm) / std::max(road_rpm, 3000.0);
            spin = clamp01(float((ratio - 0.05) / 0.35));
        }
        double angle = std::fabs(std::remainder(read_double(rdram, car + 0x3C0) - read_double(rdram, car + 0x3B8), 360.0));
        if (gear == 0 && angle > 90.0) {
            angle = 180.0 - angle;  // backing up
        }
        const float slide = clamp01(float((angle - 4.0) / 26.0)) * clamp01(float((speed - 10.0) / 20.0));
        return std::max(spin, slide);
    }
}

extern "C" void gt_slip_rumble(uint8_t* rdram, uint32_t car) {
    if (gt::get_slip_rumble() != gt::SlipRumble::On) {
        return;
    }
    int port = -1;
    for (int i = 0; i < 4; i++) {
        if (pedal_caps[i].car == car && pedal_caps[i].live) {
            port = i;
        }
    }
    if (port < 0) {
        return;
    }
    const uint8_t status = rdram[((rumble_status + port * 0x18) ^ 3) - 0x80000000u];
    const bool ready = (status >> 4) != 0 && (status & 0xF) == 0;
    const float slip = ready ? car_slip(rdram, car) : 0.0f;

    std::lock_guard lock(slip_rumble_mutex);
    SlipRumblePort& p = slip_rumble_ports[port];
    const uint32_t vi = slip_rumble_vi.load();
    p.last_vi = vi;
    // On frames this pulse: 1 from a slight slip, 2 from a medium one, 3 (half the time, like the cobbles) from 0.75.
    const int on_frames = slip < 0.08f ? 0 : std::min(3, int(std::ceil(slip * 3.0f - 0.25f)));
    p.slip_on = p.phase < on_frames;
    p.phase = (p.phase + 1) % 6;
    apply_rumble(port);

    static const bool log = std::getenv("GT_RUMBLE_LOG") != nullptr;
    if (log && slip > 0.0f) {
        std::printf("[rumble] slip port %d %.2f %s\n", port, slip, p.slip_on ? "on" : "off");
    }
}

void gt::rumble_set_game(int controller, bool on) {
    if (controller < 0 || controller >= 4) {
        recompinput::set_rumble(controller, on);
        return;
    }
    std::lock_guard lock(slip_rumble_mutex);
    slip_rumble_ports[controller].game_on = on;
    apply_rumble(controller);
}

void gt::rumble_on_vi() {
    const uint32_t vi = ++slip_rumble_vi;
    std::lock_guard lock(slip_rumble_mutex);
    for (int port = 0; port < 4; port++) {
        SlipRumblePort& p = slip_rumble_ports[port];
        if ((p.slip_on || p.phase != 0) && vi - p.last_vi > 6) {  // no race frames (paused, or the race is over)
            p = SlipRumblePort{ p.game_on, false, p.applied };
            apply_rumble(port);
        }
    }
}

// Track position (func_8003EE1C, see gt64.us.toml). Each tracked point (a car's, the camera's) keeps the track
// polygon it's on; the track is ~10-20 polygons across per section (the polygon's id, its record's u16 & 0x3FF, is the
// section: the car's +0x16A), numbered along the track. When a point leaves its polygon the game tries those up to 69
// either side, in D_800949B0's order (0 to 19 ahead first, then back), and takes the first that holds it
// (func_8003E610 returns 1 for not in it). Moving on is never more than a section a step, so a first match more than
// one section from the point's own is a polygon overlapping it from another part of the track: the nearest-section
// match that also holds it is taken instead, and with none, the point keeps its polygon (as off the track) until it's
// back on one nearby or ~1 s has passed. Matches within a section are the game's own, so normal driving is unchanged.
extern "C" void func_8003E610(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr int section_jump_limit = 1;   // sections a step
    constexpr uint32_t section_lost_limit = 90; // searches a point keeps its polygon through, rejecting only far matches
    uint8_t section_before[0x48];           // the entry before this candidate's test
    uint32_t section_entry = 0;
    uint32_t section_track = 0;
    int section_delta = 0;
    bool section_decided = false;           // this search has been resolved (section_choice)
    int section_choice = 0;                 // the candidate delta to accept, or INT_MIN for none
    struct PointState {
        uint32_t lost = 0;                  // consecutive searches with only far matches rejected
    };
    std::unordered_map<uint32_t, PointState> section_points;

    uint32_t polygon_count(uint8_t* rdram) { return rd16(rdram, section_track); }
    uint32_t polygon_id(uint8_t* rdram, uint32_t poly) { return rd16(rdram, rd32(rdram, section_track + 8) + poly * 32) & 0x3FF; }
    uint32_t polygon_of(const uint8_t* entry) { return (*reinterpret_cast<const uint32_t*>(entry) << 7) >> 21; }

    // Sections between two ids, around the lap (ids run 1 to the last).
    int section_distance(uint8_t* rdram, uint32_t a, uint32_t b) {
        static uint32_t measured = 0;
        static int lowest = 0, highest = 0;
        if (measured != section_track) {
            measured = section_track;
            lowest = 0x3FF;
            highest = 0;
            for (uint32_t i = 0; i < polygon_count(rdram); i++) {
                lowest = std::min<int>(lowest, polygon_id(rdram, i));
                highest = std::max<int>(highest, polygon_id(rdram, i));
            }
        }
        const int span = highest - lowest + 1;
        int d = std::abs(int(a) - int(b));
        return (span > 0) ? std::min(d, span - d) : d;
    }

    // A copy of the context to call a game function on: its odd float registers are reached through a pointer into
    // the context (f_odd), which has to point into the copy.
    void own_float_registers(recomp_context& copy) {
        copy.f_odd = copy.mips3_float_mode ? &copy.f1.u32l : &copy.f0.u32h;
    }

    uint32_t candidate_polygon(uint32_t from, int delta, uint32_t n) {
        return uint32_t(((int(from) + delta) % int(n) + int(n)) % int(n));
    }
}

extern "C" void gt_section_try(uint8_t* rdram, recomp_context* ctx) {
    section_entry = uint32_t(ctx->r19);
    section_delta = int16_t(rd16(rdram, uint32_t(ctx->r17)));
    section_track = rd32(rdram, uint32_t(ctx->r20) + 4);
    if (uint32_t(ctx->r18) == 0) {  // the first candidate (the point's own polygon): a new search
        section_decided = false;
    }
    std::memcpy(section_before, rdram + (section_entry - 0x80000000u), sizeof(section_before));
}

extern "C" void gt_section_result(uint8_t* rdram, recomp_context* ctx) {
    if (uint32_t(ctx->r2) == 1 || section_delta == 0 || polygon_count(rdram) == 0) {
        return;
    }
    uint8_t* const entry = rdram + (section_entry - 0x80000000u);
    const uint32_t n = polygon_count(rdram);
    const uint32_t own = polygon_of(section_before);
    const uint32_t own_id = polygon_id(rdram, own);
    PointState& point = section_points[section_entry];
    if (!section_decided) {
        section_decided = true;
        section_choice = section_delta;
        const int distance = section_distance(rdram, own_id, polygon_id(rdram, candidate_polygon(own, section_delta, n)));
        if (distance > section_jump_limit) {
            // Test the rest of this search's candidates on copies of the entry for a nearer one.
            uint8_t after[0x48];
            std::memcpy(after, entry, sizeof(after));
            const uint32_t row = 0x800949B0u + (section_before[3] >> 7) * 0x116;  // by the direction bit (word bit 31)
            int best = section_delta, best_distance = distance;
            bool past = false;
            for (uint32_t i = 0; i < 0x116 / 2; i++) {
                const int delta = int16_t(rd16(rdram, row + i * 2));
                if (!past) { past = (delta == section_delta); continue; }  // earlier ones didn't hold it
                const uint32_t poly = candidate_polygon(own, delta, n);
                const int d = section_distance(rdram, own_id, polygon_id(rdram, poly));
                if (d >= best_distance) continue;
                std::memcpy(entry, section_before, sizeof(section_before));
                recomp_context test = *ctx;
                own_float_registers(test);
                test.r4 = int32_t(rd32(rdram, section_track + 4));
                test.r5 = int32_t(rd32(rdram, section_track + 8) + poly * 32);
                test.r6 = int32_t(section_entry);
                func_8003E610(rdram, &test);
                if (uint32_t(test.r2) != 1) {
                    best = delta;
                    best_distance = d;
                }
            }
            std::memcpy(entry, after, sizeof(after));
            if (best_distance > section_jump_limit && point.lost < section_lost_limit) {
                section_choice = INT32_MIN;  // keep its polygon
                point.lost++;
            } else {
                section_choice = best;
            }
            static uint32_t logged = 0;
            if (logged++ < 50) {
                std::fprintf(stderr, "[track] f%u point %08X section %u: first match %+d (%d sections) -> %s %+d\n", game_frame,
                    section_entry, own_id, section_delta, distance, (section_choice == INT32_MIN) ? "kept, best" : "took", best);
            }
        }
        if (section_choice != INT32_MIN && section_distance(rdram, own_id, polygon_id(rdram, candidate_polygon(own, section_choice, n))) <= section_jump_limit) {
            point.lost = 0;
        }
    }
    if (section_delta != section_choice) {
        std::memcpy(entry, section_before, sizeof(section_before));
        ctx->r2 = 1;
    }
}

// Reduce AI Aggression (General > Reduce AI Aggression). Opponents don't drive with the car physics: each step of
// the car update, func_80028520 runs one along its line (its record, D_80162758 + (id - 1) * 0xA8, has its speed in units per
// move at +0x54) and func_800276E4 then moves it that far. Just before the move, an opponent within a section of a
// player's car that's ahead of it and less than two car widths to the side of its heading (its last move) is held to
// the player's own speed along it (less what it'd lose in a few more moves if it's braking), plus what closes in to
// ai_gap over ai_horizon moves: it brakes with the player rather than drive into it. Held back to under half its speed for over ~3 s (the player stopped on
// its line), it's let go. Once no longer held back, it's eased back up to its own speed over ai_recover_frames rather
// than the game's quicker acceleration.
namespace {
    constexpr double ai_gap = 360.0;     // from the opponent's centre to the player's, along its heading (a car is ~230 long)
    constexpr double ai_width = 200.0;   // to the side: two car widths (a car is ~100 wide)
    constexpr double ai_horizon = 12.0;  // moves to close in to ai_gap over (the game moves opponents a few times a frame)
    constexpr double ai_brake_lookahead = 4.0;  // moves of the player's braking assumed to continue
    constexpr uint32_t ai_hold_limit = 90; // frames (3 s)
    constexpr double ai_recover_frames = 90.0;  // frames to ease back up from a standstill to its own speed (3 s)
    struct AiState {
        double x = 0.0, z = 0.0;         // position before this frame's move
        double dx = 0.0, dz = 0.0;       // heading: the last move, normalised
        bool heading = false;
        uint32_t held_since = 0;         // the frame (game_frame) it was first held back to under half its speed, or 0
        double eased = -1.0;             // the speed it was last held back to (or -1: not easing back up)
        uint32_t eased_frame = 0;        // the frame that was on
        uint32_t moves = 0;              // this opponent's moves so far
        double px[9] = {}, pz[9] = {};   // each player's position at its previous move, by id
        double pv[9] = {};               // and the player's speed along its heading then
        uint32_t seen[9] = {};           // the move (moves) those were taken on
    };
    std::unordered_map<uint32_t, AiState> ai_states;   // by object

    uint32_t object_of_id(uint8_t* rdram, int id) {
        const uint32_t base = rd32(rdram, 0x800DB5DCu);
        if (base < 0x80000000u || base >= 0x80800000u) return 0;
        for (uint32_t i = 0; i < 8; i++) {
            const uint32_t obj = base + i * 0x1A0;
            if (rd16(rdram, obj + 0x22) == uint32_t(id)) return obj;
        }
        return 0;
    }
}

extern "C" void gt_ai_before_move(uint8_t* rdram, uint32_t rec) {
    const uint32_t obj = rd32(rdram, 0x800DB5D0u);
    if (obj < 0x80000000u || obj >= 0x80800000u) return;
    AiState& ai = ai_states[obj];
    const double ax = rd64f(rdram, obj + 0x40), az = rd64f(rdram, obj + 0x50);
    ai.x = ax;
    ai.z = az;
    if (gt::get_reduce_ai_aggression() != gt::ReduceAiAggression::On || !ai.heading) {
        ai.eased = -1.0;
        return;
    }
    const float speed = rd32f(rdram, rec + 0x54);
    if (!(speed > 0.0f)) return;
    const int ring = std::max(section_ring(rdram), 1);
    const int ai_section = rd16(rdram, obj + 0x16A);
    double allowed = speed;
    double nearest_gap = 0.0;
    int player_id = 0;
    ai.moves++;
    for (int id = 1; id <= 8; id++) {
        const uint32_t player = object_of_id(rdram, id);
        if (player == 0 || player == obj || !controller_car(0x80162E20u + uint32_t(id - 1) * 0x3F0u)) continue;
        const double px = rd64f(rdram, player + 0x40), pz = rd64f(rdram, player + 0x50);
        // The player's own move since this opponent's previous one, along its heading.
        double player_speed = 0.0;
        bool tracked = false;
        if (ai.seen[id] + 1 == ai.moves) {
            const double mx = px - ai.px[id], mz = pz - ai.pz[id];
            if (mx * mx + mz * mz < 500.0 * 500.0) {
                player_speed = std::max(0.0, mx * ai.dx + mz * ai.dz);  // not slowed further for a player coming towards it
                tracked = true;
            }
        }
        // A player braking: the speed it'll be down to a few moves on if it keeps braking, so the opponent brakes with
        // it rather than a move or more behind.
        const double slowing = (tracked && ai.seen[id] != 0) ? std::max(0.0, ai.pv[id] - player_speed) : 0.0;
        const double expected_speed = std::max(0.0, player_speed - slowing * ai_brake_lookahead);
        ai.px[id] = px;
        ai.pz[id] = pz;
        ai.pv[id] = tracked ? player_speed : 0.0;
        ai.seen[id] = ai.moves;
        int dsec = std::abs(int(rd16(rdram, player + 0x16A)) - ai_section);
        dsec = std::min(dsec, ring - dsec);
        if (dsec > 1) continue;
        const double rx = px - ax, rz = pz - az;
        const double along = rx * ai.dx + rz * ai.dz;
        const double side = std::fabs(rx * ai.dz - rz * ai.dx);
        if (along <= 0.0 || side > ai_width) continue;
        // As fast as keeps it from closing in to under ai_gap within ai_horizon moves.
        const double keep = std::max(0.0, expected_speed + (along - ai_gap) / ai_horizon);
        if (keep < allowed) {
            allowed = keep;
            nearest_gap = along;
            player_id = id;
        }
    }
    if (allowed >= speed * 0.5) {
        ai.held_since = 0;
    }
    else if (ai.held_since == 0) {
        ai.held_since = game_frame;
    }
    else if (game_frame - ai.held_since > ai_hold_limit) {
        allowed = speed;  // let go
    }
    if (allowed < speed) {
        ai.eased = allowed;
        ai.eased_frame = game_frame;
    }
    else if (ai.eased >= 0.0) {
        // Easing back up: from the speed it was held to, gaining its own speed's worth over ai_recover_frames.
        const double cap = ai.eased + speed * double(game_frame - ai.eased_frame) / ai_recover_frames;
        if (cap >= speed) {
            ai.eased = -1.0;
            return;
        }
        allowed = cap;
    }
    else {
        return;
    }
    static const bool log = std::getenv("GT_AIBRAKE_LOG") != nullptr;
    if (log) {
        std::fprintf(stderr, "[ai] f%u car %d behind player %d by %.0f: speed %.1f -> %.1f (held since f%u)\n", game_frame,
            int(rd16(rdram, obj + 0x22)), player_id, nearest_gap, speed, allowed, ai.held_since);
    }
    wr32f(rdram, rec + 0x54, float(allowed));
}

// After an opponent's move (func_80028604's start): its heading for the next frame.
extern "C" void gt_ai_after_move(uint8_t* rdram) {
    const uint32_t obj = rd32(rdram, 0x800DB5D0u);
    auto it = ai_states.find(obj);
    if (it == ai_states.end()) return;
    AiState& ai = it->second;
    const double mx = rd64f(rdram, obj + 0x40) - ai.x, mz = rd64f(rdram, obj + 0x50) - ai.z;
    const double len = std::sqrt(mx * mx + mz * mz);
    // A jump (a new race, a reset) leaves no heading; standing still (held back) keeps the last one.
    if (len >= 500.0) {
        ai.heading = false;
        ai.held_since = 0;
        ai.eased = -1.0;
    }
    else if (len > 0.5) {
        ai.heading = true;
        ai.dx = mx / len;
        ai.dz = mz / len;
    }
}


// Traction Control (General > Traction Control). Each physics step, after the game updates a controller car's throttle
// pedal (gt_pedal_end), the pedal is held down to what keeps the car from slipping: less from 1% of wheelspin (its
// driven wheels' revs past the revs its road speed gives, car +0x168 against +0x100), none from 5%, and less from 1
// degree of slide (the angle between its heading, +0x3B8, and where it's going, +0x3C0), none from 6. The game's own
// ramp brings it back up as grip returns. The brake is left to the player.
constexpr double tc_spin_start = 0.01, tc_spin_full = 0.05;
constexpr double tc_slide_start = 1.0, tc_slide_full = 6.0;   // degrees

void gt_traction_control(uint8_t* rdram, uint32_t car, bool brake) {
    if (brake || gt::get_traction_control() != gt::TractionControl::On) {
        return;
    }
    const int gear = read_half(rdram, car + 0x178);
    const double speed = std::fabs(read_double(rdram, car + 0x110));   // km/h
    double angle = std::fabs(std::remainder(read_double(rdram, car + 0x3C0) - read_double(rdram, car + 0x3B8), 360.0));
    if (gear == 0 && angle > 90.0) {
        angle = 180.0 - angle;  // backing up
    }
    if (gear == 1) {
        return;  // neutral
    }
    const double road_rpm = read_double(rdram, car + 0x100);
    const double wheel_rpm = read_double(rdram, car + 0x168);
    const double spin = (wheel_rpm - road_rpm) / std::max(road_rpm, 3000.0);
    float cap = clamp01(float(1.0 - (spin - tc_spin_start) / (tc_spin_full - tc_spin_start)));
    if (speed > 15.0) {
        cap = std::min(cap, clamp01(float(1.0 - (angle - tc_slide_start) / (tc_slide_full - tc_slide_start))));
    }
    if (read_double(rdram, car + 0x180) > double(cap)) {
        wr64f(rdram, car + 0x180, double(cap));
    }
    static const bool log = std::getenv("GT_TC_LOG") != nullptr;
    if (log) {
        static uint32_t n = 0;
        if ((n++ % 4) == 0) {
            std::fprintf(stderr, "[tc] f%u gear %d speed %.1f spin %.3f angle %.1f thr %.2f brk %.2f cap %.2f\n", game_frame, gear, speed, spin,
                angle, read_double(rdram, car + 0x180), read_double(rdram, car + 0x188), cap);
        }
    }
}

// Barrier containment (see gt64.us.toml). The game tracks a car's centre and four corner points over the track's
// polygons (func_8003EE1C): a point whose move crosses a wall edge is stopped at it (the point's entry: target x/z at
// +0x20/+0x28, tracked x/z at +0x30/+0x40, the wall's angle at +4 or -1), and func_80039AA4 then turns or pushes the
// car by which corners were stopped. Only the tracked points are held by walls, not the car.
namespace {
    struct WallPoint {
        double tx, tz, cx, cz;
        float wall;
    };
    WallPoint wall_point(uint8_t* rdram, uint32_t entry) {
        return { rd64f(rdram, entry + 0x20), rd64f(rdram, entry + 0x28), rd64f(rdram, entry + 0x30), rd64f(rdram, entry + 0x40),
            rd32f(rdram, entry + 4) };
    }
    uint32_t wall_rand_state = 12345;
    double wall_rand() {
        wall_rand_state = wall_rand_state * 1664525u + 1013904223u;
        return double(wall_rand_state >> 8) / double(1u << 24);
    }
    // Whether a position is on any of the track's polygons (track: the car's tracker's, at its draw group's +0x64).
    void track_polygon(uint8_t* rdram, uint32_t track, uint32_t i, double* vx, double* vz, uint32_t& nv) {
        const uint32_t verts = rd32(rdram, track + 4), poly = rd32(rdram, track + 8) + i * 32;
        nv = rd32(rdram, poly) & 0xF;
        for (uint32_t k = 0; k < nv; k++) {
            const uint32_t v = rd16(rdram, poly + 4 + k * 2);
            vx[k] = rd64f(rdram, 0x80094990u + (v >> 14) * 8) + rd32f(rdram, verts + (v & 0xFFF) * 8);
            vz[k] = rd64f(rdram, 0x80094990u + ((v >> 12) & 3) * 8) + rd32f(rdram, verts + (v & 0xFFF) * 8 + 4);
        }
    }
    bool on_track(uint8_t* rdram, uint32_t track, double x, double z) {
        const uint32_t count = rd16(rdram, track);
        for (uint32_t i = 0; i < count; i++) {
            double vx[16], vz[16];
            uint32_t nv;
            track_polygon(rdram, track, i, vx, vz, nv);
            if (nv < 3) continue;
            bool in = true;
            for (uint32_t k = 0; k < nv && in; k++) {
                const uint32_t m = (k + 1) % nv;
                in = (z - vz[k]) * (vx[m] - vx[k]) - (vz[m] - vz[k]) * (x - vx[k]) >= 0.0;
            }
            if (in) return true;
        }
        return false;
    }
    std::string wall_history[6];
    uint32_t wall_history_at = 0;
    uint32_t wall_steps = 0;
    int wall_test_left = 0;
    double wall_test_travel = 0.0, wall_test_heading = 0.0, wall_test_speed = 0.0, wall_test_spin = 0.0;
}

// Debug (GT_WALLTEST=<steps>): every <steps> physics steps, a player's car is shoved for 40 steps: at a random speed in
// a random direction, turned sideways, backwards or at random to it.
extern "C" void gt_wall_test(uint8_t* rdram, uint32_t car) {
    static const int every = std::getenv("GT_WALLTEST") ? std::atoi(std::getenv("GT_WALLTEST")) : 0;
    if (every <= 0 || car < 0x80000000u || car >= 0x80800000u || !controller_car(car)) return;
    const uint32_t obj = rd32(rdram, 0x800DB5D0u);
    const double x = rd64f(rdram, obj + 0x40), z = rd64f(rdram, obj + 0x50);
    static double last_x = 0.0, last_z = 0.0;
    if (wall_steps == 0 && std::getenv("GT_WALLSEED")) wall_rand_state = uint32_t(std::atoi(std::getenv("GT_WALLSEED")));
    static const bool teleport = std::getenv("GT_WALLTEST_TELEPORT") != nullptr;
    const uint32_t group = rd32(rdram, car + 0x3E8);
    if ((wall_steps + 1) % uint32_t(every) == 0 && teleport && group >= 0x80000000u && group < 0x80800000u) {
        const uint32_t track = rd32(rdram, group + 0x64);
        static uint32_t trials = 0, escapes = 0;
        if (trials != 0 && !on_track(rdram, track, x, z)) {
            escapes++;
            std::fprintf(stderr, "[walltest] ESCAPE %u of %u trials: at %.0f %.0f\n", escapes, trials, x, z);
            for (uint32_t i = 0; i < 6; i++) std::fprintf(stderr, "      %s\n", wall_history[(wall_history_at + i) % 6].c_str());
        }
        trials++;
        // Onto a random polygon with a wall, at its centre.
        const uint32_t count = rd16(rdram, track);
        uint32_t poly = 0, nv = 0;
        double vx[16], vz[16];
        for (int tries = 0; tries < 200; tries++) {
            poly = uint32_t(wall_rand() * count) % count;
            if ((rd16(rdram, rd32(rdram, track + 8) + poly * 32 + 2) & 0xF0) != 0) break;
        }
        track_polygon(rdram, track, poly, vx, vz, nv);
        double px = 0.0, pz = 0.0;
        for (uint32_t k = 0; k < nv; k++) { px += vx[k] / nv; pz += vz[k] / nv; }
        for (const uint32_t at : { obj + 0x40, obj + 0x120, car + 0x8 + 0x78, car + 0xB0 + 0x78 }) {
            wr64f(rdram, at, px);
            wr64f(rdram, at + 0x10, pz);
        }
        uint32_t entries[5] = { rd32(rdram, group + 0x60 + 8), 0, 0, 0, 0 };
        for (uint32_t i = 0; i < 4; i++) entries[i + 1] = rd32(rdram, rd32(rdram, car + 0x3D8 + i * 4) + 0x68 + 8);
        for (const uint32_t entry : entries) {
            if (entry < 0x80000000u || entry >= 0x80800000u) continue;
            wr32(rdram, entry, (rd32(rdram, entry) & ~(0x7FFu << 14)) | (poly << 14));
            wr64f(rdram, entry + 0x20, px);
            wr64f(rdram, entry + 0x28, pz);
            wr64f(rdram, entry + 0x30, px);
            wr64f(rdram, entry + 0x40, pz);
        }
        std::fprintf(stderr, "[walltest] trial %u: polygon %u at %.0f %.0f\n", trials, poly, px, pz);
    }
    if (++wall_steps % uint32_t(every) == 0) {
        wall_test_left = 40;
        wall_test_travel = wall_rand() * 360.0;
        const int kind = int(wall_rand() * 5.0);
        const double turn = (kind == 0) ? 90.0 : (kind == 1) ? -90.0 : (kind == 2) ? 0.0 : (kind == 3) ? 180.0 : wall_rand() * 360.0;
        wall_test_heading = std::fmod(wall_test_travel + turn + 720.0, 360.0);
        wall_test_speed = 15.0 + wall_rand() * 65.0;   // units a step (about m/s)
        static const double spin = std::getenv("GT_WALLTEST_SPIN") ? std::atof(std::getenv("GT_WALLTEST_SPIN")) : 0.0;
        wall_test_spin = (wall_rand() * 2.0 - 1.0) * spin;
        std::fprintf(stderr, "[walltest] f%u step %u: shove travel %.1f heading %.1f speed %.0f spin %.1f\n", game_frame, wall_steps, wall_test_travel,
            wall_test_heading, wall_test_speed, wall_test_spin);
    }
    if (wall_test_left > 0) {
        if (wall_test_left < 38 && wall_test_left > 34) std::fprintf(stderr, "[walltest]   moved %.1f a step at %.0f (%.0f)\n", std::hypot(x - last_x, z - last_z), rd64f(rdram, car + 0x110), rd64f(rdram, car + 0x1E8));
        wall_test_left--;
    }
    last_x = x;
    last_z = z;
}

// Debug: before the car's move (func_8002E574).
extern "C" void gt_wall_test_move(uint8_t* rdram, uint32_t car) {
    static const int hold = std::getenv("GT_WALLTEST_HOLD") ? std::atoi(std::getenv("GT_WALLTEST_HOLD")) : 40;
    if (wall_test_left <= 40 - hold || car < 0x80000000u || car >= 0x80800000u || !controller_car(car)) return;
    wr64f(rdram, car + 0x3C0, wall_test_travel);
    static const bool turn_once = std::getenv("GT_WALLTEST_TURNONCE") != nullptr;
    if (!turn_once || wall_test_left == 39) wr64f(rdram, car + 0x3B8, wall_test_heading);
    else if (wall_test_spin != 0.0) wr64f(rdram, car + 0x3B8, std::fmod(rd64f(rdram, car + 0x3B8) + wall_test_spin + 720.0, 360.0));
    wr64f(rdram, car + 0x1E8, wall_test_speed);
}

// After a car's wall response (func_80039AA4): a corner still past a wall (its target beyond where it was stopped) has
// the car moved back out of the wall by how far it's through, along the wall's normal, and the corners are tracked
// again (func_80038508), up to wall_passes times (for a corner between two walls). The game's own response only turns
// the car or pushes it along its length by a few fixed steps, and does nothing for some sets of corners (opposite
// ones, all four), so a car sliding sideways into a barrier went through it: once all four corners were past the
// wall it drove on outside the track. Only for a player's car.
extern "C" void func_80038508(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr int wall_passes = 6;
    constexpr int wall_normal_passes = 3;    // out along the wall's normal first, then straight back
    constexpr double wall_depth_threshold = 40.0;  // units through a wall (a car is ~100 wide) before the car's moved back
    constexpr double wall_margin = 0.5;      // units inside the wall the corner is left
    constexpr double wall_shift_limit = 600.0;  // a corner further through than this isn't a move into a wall (the car was placed)

    void track_corners(uint8_t* rdram, recomp_context* ctx, uint32_t car) {
        recomp_context call = *ctx;
        own_float_registers(call);
        const uint32_t group = rd32(rdram, car + 0x3E8);
        call.r4 = 0;
        call.r5 = int32_t((rd32(rdram, group + 0x18) << 7) >> 21);   // the centre's polygon, as func_80032874 passes
        func_80038508(rdram, &call);
    }
}

extern "C" void gt_wall_after(uint8_t* rdram, recomp_context* ctx, uint32_t car) {
    if (car < 0x80000000u || car >= 0x80800000u || !controller_car(car)) return;
    static const bool log = std::getenv("GT_WALLLOG") != nullptr;
    static const bool off = std::getenv("GT_WALLOFF") != nullptr;
    const uint32_t obj = rd32(rdram, 0x800DB5D0u);
    const uint32_t group_at = rd32(rdram, car + 0x3E8);
    if (obj < 0x80000000u || obj >= 0x80800000u || group_at < 0x80000000u || group_at >= 0x80800000u) return;
    uint32_t corners[4];
    bool any = false;
    for (uint32_t i = 0; i < 4; i++) {
        corners[i] = rd32(rdram, car + 0x3D8 + i * 4);
        if (corners[i] < 0x80000000u || corners[i] >= 0x80800000u) return;
        any = any || rd32f(rdram, corners[i] + 0x24) >= 0.0f;
    }
    if (log) {
        char text[800];
        int n = std::snprintf(text, sizeof(text), "step %u f%u speed %.0f:", wall_steps, game_frame, rd64f(rdram, car + 0x1E8));
        for (uint32_t i = 0; i < 4; i++) {
            const WallPoint p = wall_point(rdram, corners[i] + 0x20);
            n += std::snprintf(text + n, sizeof(text) - n, " [%u %08X t %.2f %.2f c %.2f %.2f w %.1f]", i, rd32(rdram, corners[i] + 0x20), p.tx, p.tz, p.cx, p.cz, p.wall);
        }
        {
            const WallPoint c = wall_point(rdram, group_at + 0x18);
            n += std::snprintf(text + n, sizeof(text) - n, " centre %08X obj %.1f %.1f c %.1f %.1f w %.1f", rd32(rdram, group_at + 0x18), rd64f(rdram, obj + 0x40),
                rd64f(rdram, obj + 0x50), c.cx, c.cz, c.wall);
        }
        wall_history[wall_history_at++ % 6] = text;
    }
    // Debug: the track's polygons to a file (GT_TRACKDUMP), the car's path to the log (GT_WALLTRAIL).
    if (log) {
        const uint32_t track = rd32(rdram, group_at + 0x64);
        static bool dumped = false;
        if (!dumped && std::getenv("GT_TRACKDUMP")) {
            dumped = true;
            if (FILE* f = std::fopen(std::getenv("GT_TRACKDUMP"), "w")) {
                for (uint32_t i = 0; i < rd16(rdram, track); i++) {
                    double vx[16], vz[16];
                    uint32_t nv;
                    track_polygon(rdram, track, i, vx, vz, nv);
                    std::fprintf(f, "%u %08X %04X", i, rd32(rdram, rd32(rdram, track + 8) + i * 32), rd16(rdram, rd32(rdram, track + 8) + i * 32 + 2));
                    for (uint32_t k = 0; k < nv; k++) std::fprintf(f, " %.1f %.1f", vx[k], vz[k]);
                    std::fprintf(f, "\n");
                }
                std::fclose(f);
            }
        }
        static const bool trail = std::getenv("GT_WALLTRAIL") != nullptr;
        if (trail) {
            const double x = rd64f(rdram, obj + 0x40), z = rd64f(rdram, obj + 0x50);
            std::fprintf(stderr, "[trail] %u %.1f %.1f %.1f %d\n", wall_steps, x, z, rd64f(rdram, car + 0x3B8), int(on_track(rdram, track, x, z)));
        }
    }
    if (!any && !off) return;
    double moved_x = 0.0, moved_z = 0.0;
    int passes = 0;
    double worst_left = 0.0;
    int mask = 0;
    for (int pass = 0; pass <= wall_passes && !off; pass++) {
        track_corners(rdram, ctx, car);   // the response only tracks the corners it's working on
        double worst = 0.0, sx = 0.0, sz = 0.0, furthest = 0.0;
        mask = 0;
        for (uint32_t i = 0; i < 4; i++) {
            const WallPoint p = wall_point(rdram, corners[i] + 0x20);
            if (p.wall < 0.0f) continue;
            mask |= 1 << i;
            const double out = std::hypot(p.cx - p.tx, p.cz - p.tz);
            if (out < 1e-6) continue;   // within the wall's reach, not through it
            furthest = std::max(furthest, out);
            // The wall's inward normal, from its angle (atan2(dz, dx) of the edge, in degrees), and how far through.
            const double a = double(p.wall) * 3.14159265358979323846 / 180.0;
            const double nx = -std::sin(a), nz = std::cos(a);
            const double depth = std::max(0.0, (p.cx - p.tx) * nx + (p.cz - p.tz) * nz);
            if (pass < wall_normal_passes) {
                if (depth >= worst) { worst = std::max(depth, 1e-3); sx = nx * (depth + wall_margin); sz = nz * (depth + wall_margin); }
            }
            else if (out >= worst) {
                // Still through a wall: straight back to where the corner was stopped.
                worst = out;
                sx = (p.cx - p.tx) * (1.0 + wall_margin / out);
                sz = (p.cz - p.tz) * (1.0 + wall_margin / out);
            }
        }
        worst_left = worst;
        if (worst <= 0.0 || furthest > wall_shift_limit || pass == wall_passes) break;
        // A corner a little through a wall is the game's ordinary contact, which its own response settles: left to it,
        // unless it's a set of corners the game does nothing for (opposite ones, all four).
        if (pass == 0 && furthest < wall_depth_threshold && mask != 5 && mask != 10 && mask != 15) break;
        for (const uint32_t at : { obj + 0x40, obj + 0x120, car + 0x8 + 0x78, car + 0xB0 + 0x78 }) {
            wr64f(rdram, at, rd64f(rdram, at) + sx);
            wr64f(rdram, at + 0x10, rd64f(rdram, at + 0x10) + sz);
        }
        moved_x += sx;
        moved_z += sz;
        passes++;
    }
    static uint32_t logged = 0;
    if (log && (passes != 0 || worst_left > 0.0) && logged++ < 20000) {
        std::fprintf(stderr, "[wall] f%u mask %X moved %.1f %.1f in %d, left %.1f; speed %.0f heading %.0f travel %.0f\n", game_frame, mask,
            moved_x, moved_z, passes, worst_left, rd64f(rdram, car + 0x110), rd64f(rdram, car + 0x3B8), rd64f(rdram, car + 0x3C0));
    }
}
