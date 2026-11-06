// Native helpers called from recompiler hooks (gt64.us.toml) to implement enhancements.
#include <cstdint>
#include <cstring>
#include <map>

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

namespace RT64 { void setCardRemap(bool enabled); void setFullFrameView(bool enabled); void setCardRemapBox(int32_t x0, int32_t y0, int32_t x1, int32_t y1); void setRaceHoleFill(bool enabled); void setOutputBrightness(float brightness); }

namespace {
    constexpr uint32_t mode_func_ptr = 0x800E8810;
    constexpr uint32_t generic_mode = 0x800018A8;  // between modes: keep the previous state
    constexpr uint32_t intro_mode = 0x80003540;
    // Modes from the round menu's exit on (pre-race, loading, the race, its pause menu, results...): widescreen while
    // they draw a race (a full-frame 3D view).
    constexpr uint32_t race_modes_start = 0x80004240;
    uint32_t full_view_vi = 0;  // last VI on which a full-frame 3D view was built
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
    {
        static const bool log = std::getenv("GT_MODELOG") != nullptr;
        static int last = -1;
        if (log && int(race) != last) { fprintf(stderr, "[race] %d mode=%08X t=%.2f\n", int(race), mode, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()); last = race; }
    }
    RT64::setRaceHoleFill(race);
    // Into a race, the game cross-fades from the last 4:3 frame (the start card) to the race, so switching the aspect
    // ratio as the race appears stretched the card for a moment. Fade the output through black instead: fade out
    // (still pillarboxed) as the card ends, hold black through loading until the race mode has begun and the
    // cross-fade is over, switch, fade in.
    constexpr uint32_t race_mode = 0x800046C4;
    // The cross-fade lasts about 20 game frames: hold for 24 frames from the first race frame (and at least ~0.5 s).
    constexpr uint32_t fade_out_vis = 24, hold_min_vis = 24, hold_max_vis = 300, fade_in_vis = 15, crossfade_frames = 24;
    static int transition = 0;  // 0 none, 1 fading out, 2 black, 4 black after the switch, 3 fading in
    static uint32_t transition_vi = 0;
    static uint32_t transition_swap = 0;
    static bool race_seen = false;  // the race has started drawing since the fade began
    // Start fading out as the start card ends (the loading modes after it), before the game starts its cross-fade.
    constexpr uint32_t card_end_mode = 0x800042E8;
    const bool loading = (mode >= card_end_mode) && (mode <= race_mode);
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

extern "C" void gt_controller_poll(uint8_t* rdram) {
    controller_polls++;
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

extern "C" void gt_pedal_end(uint8_t* rdram, int brake) {
    for (const PedalCap& cap : pedal_caps) {
        if (cap.car != 0 && cap.car == pedal_car) {
            cap_pedal(rdram, pedal_car + (brake ? 0x188 : 0x180), brake ? cap.brake : cap.throttle);
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
