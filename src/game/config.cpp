#include <filesystem>

#include "recompui/config.h"
#include "recompinput/input_mapping.h"
#include "util/file.h"

#include "ultramodern/config.hpp"

#include "gt_config.h"

static void add_graphics_options(recomp::config::Config &config) {
    using EnumOptionVector = const std::vector<recomp::config::ConfigOptionEnumOption>;

    static EnumOptionVector frame_rate_options = {
        {gt::FrameRate::Original, "Original", "Original"},
        {gt::FrameRate::Fps60, "60", "60 FPS"},
    };
    config.add_enum_option(
        gt::configkeys::graphics::frame_rate,
        "Frame Rate",
        "<recomp-color primary>60 FPS</recomp-color> (experimental) lifts the 30 FPS cap the game uses in races and some menus. "
        "<recomp-color primary>Original</recomp-color> matches the N64.",
        frame_rate_options,
        gt::FrameRate::Original,
        // Hidden: lifting the game's 30 FPS cap makes races and the intro run too fast (part of the game logic counts
        // frames). Kept for development.
        true
    );
}

static void add_draw_distance_option(recomp::config::Config &config) {
    using EnumOptionVector = const std::vector<recomp::config::ConfigOptionEnumOption>;
    static EnumOptionVector draw_distance_options = {
        {gt::DrawDistance::Original, "Original", "Original"},
        {gt::DrawDistance::Extended, "Extended", "Extended"},
        {gt::DrawDistance::Maximum, "Maximum", "Maximum"},
    };
    config.add_enum_option(
        gt::configkeys::graphics::draw_distance,
        "Draw Distance",
        "How much of the track is drawn in races. <recomp-color primary>Extended</recomp-color> draws about twice as far "
        "ahead, <recomp-color primary>Maximum</recomp-color> as far as the engine geometry can reach; both keep the most detailed models at any "
        "distance. Fog and weather are unchanged. <recomp-color primary>Original</recomp-color> matches the N64.",
        draw_distance_options,
        gt::DrawDistance::Maximum
    );
}

static void add_general_options(recomp::config::Config &config) {
    using EnumOptionVector = const std::vector<recomp::config::ConfigOptionEnumOption>;

    static EnumOptionVector analog_trigger_options = {
        {gt::AnalogTriggers::Off, "Off", "Off"},
        {gt::AnalogTriggers::On, "On", "On"},
    };
    config.add_enum_option(
        gt::configkeys::general::analog_triggers,
        "Analog Triggers",
        "<recomp-color primary>On</recomp-color> makes the throttle and brake follow how far you press the triggers "
        "(or any analog input bound to A and B). <recomp-color primary>Off</recomp-color> is the N64's on/off buttons.",
        analog_trigger_options,
        gt::AnalogTriggers::On
    );

    static EnumOptionVector auto_gearbox_options = {
        {gt::AutoGearbox::Original, "Original", "Original"},
        {gt::AutoGearbox::Custom, "Custom", "RobotBox"},  // saved as Custom
    };
    config.add_enum_option(
        gt::configkeys::general::auto_gearbox,
        "Automatic Gearbox",
        "How the automatic transmission shifts. <recomp-color primary>RobotBox</recomp-color> is custom automatic "
        "gearbox logic inspired by RobotBox for Assetto Corsa, and gave faster lap times in testing. "
        "<recomp-color primary>Original</recomp-color> keeps the game's original shift logic.",
        auto_gearbox_options,
        gt::AutoGearbox::Original
    );

    static EnumOptionVector slip_rumble_options = {
        {gt::SlipRumble::Off, "Off", "Off"},
        {gt::SlipRumble::On, "On", "On"},
    };
    config.add_enum_option(
        gt::configkeys::general::slip_rumble,
        "Slip Rumble",
        "<recomp-color primary>On</recomp-color> adds rumble while your wheels spin or the car slides, stronger the "
        "more it slips. <recomp-color primary>Off</recomp-color> keeps the game's original rumble.",
        slip_rumble_options,
        gt::SlipRumble::On
    );

    static EnumOptionVector reduce_ai_aggression_options = {
        {gt::ReduceAiAggression::Off, "Off", "Off"},
        {gt::ReduceAiAggression::On, "On", "On"},
    };
    config.add_enum_option(
        gt::configkeys::general::reduce_ai_aggression,
        "Reduce AI Aggression",
        "<recomp-color primary>On</recomp-color>: Opponents brake instead of driving into your car. "
        "<recomp-color primary>Off</recomp-color>: Keeps the game's original AI.",
        reduce_ai_aggression_options,
        gt::ReduceAiAggression::On
    );

    static EnumOptionVector traction_control_options = {
        {gt::TractionControl::Off, "Off", "Off"},
        {gt::TractionControl::On, "On", "On"},
    };
    config.add_enum_option(
        gt::configkeys::general::traction_control,
        "Traction Control",
        "<recomp-color primary>On</recomp-color>: Adjusts throttle when slip is detected to minimise wheelspin and oversteer. "
        "<recomp-color primary>Off</recomp-color>: Keeps the game's original handling.",
        traction_control_options,
        gt::TractionControl::Off
    );
}

gt::SlipRumble gt::get_slip_rumble() {
    return static_cast<gt::SlipRumble>(std::get<uint32_t>(recompui::config::get_general_config().get_option_value(gt::configkeys::general::slip_rumble)));
}

gt::TractionControl gt::get_traction_control() {
    return static_cast<gt::TractionControl>(std::get<uint32_t>(recompui::config::get_general_config().get_option_value(gt::configkeys::general::traction_control)));
}

gt::ReduceAiAggression gt::get_reduce_ai_aggression() {
    return static_cast<gt::ReduceAiAggression>(std::get<uint32_t>(recompui::config::get_general_config().get_option_value(gt::configkeys::general::reduce_ai_aggression)));
}

gt::AutoGearbox gt::get_auto_gearbox() {
    return static_cast<gt::AutoGearbox>(std::get<uint32_t>(recompui::config::get_general_config().get_option_value(gt::configkeys::general::auto_gearbox)));
}

gt::AnalogTriggers gt::get_analog_triggers() {
    return static_cast<gt::AnalogTriggers>(std::get<uint32_t>(recompui::config::get_general_config().get_option_value(gt::configkeys::general::analog_triggers)));
}

template <typename T = uint32_t>
T get_graphics_config_enum_value(const std::string& option_id) {
    return static_cast<T>(std::get<uint32_t>(recompui::config::get_graphics_config().get_option_value(option_id)));
}

static void add_hd_wheels_option(recomp::config::Config &config) {
    using EnumOptionVector = const std::vector<recomp::config::ConfigOptionEnumOption>;
    static EnumOptionVector hd_wheels_options = {
        {gt::HdWheels::Off, "Off", "Off"},
        {gt::HdWheels::On, "On", "On"},
    };
    config.add_enum_option(
        gt::configkeys::graphics::hd_wheels,
        "HD Wheels",
        "<recomp-color primary>On</recomp-color> replaces the cars' six-sided wheels with modelled ones: round tyres "
        "and five-spoke alloy rims. <recomp-color primary>Off</recomp-color> keeps the original wheels.",
        hd_wheels_options,
        gt::HdWheels::On
    );
}

static void add_depth_of_field_option(recomp::config::Config &config) {
    using EnumOptionVector = const std::vector<recomp::config::ConfigOptionEnumOption>;
    static EnumOptionVector depth_of_field_options = {
        {gt::DepthOfField::Off, "Off", "Off"},
        {gt::DepthOfField::On, "On", "On"},
    };
    config.add_enum_option(
        gt::configkeys::graphics::depth_of_field,
        "Depth of Field",
        "<recomp-color primary>On</recomp-color> blurs the distance past where the original draw distance ended (with "
        "<recomp-color primary>Extended</recomp-color> or <recomp-color primary>Maximum</recomp-color> draw distance). "
        "<recomp-color primary>Off</recomp-color> keeps it all sharp.",
        depth_of_field_options,
        gt::DepthOfField::On
    );
}

static void add_motion_blur_option(recomp::config::Config &config) {
    using EnumOptionVector = const std::vector<recomp::config::ConfigOptionEnumOption>;
    static EnumOptionVector motion_blur_options = {
        {gt::MotionBlur::Off, "Off", "Off"},
        {gt::MotionBlur::On, "On", "On"},
    };
    config.add_enum_option(
        gt::configkeys::graphics::motion_blur,
        "Motion Blur",
        "<recomp-color primary>On</recomp-color> blurs the track and scenery in races as the camera moves past them; the cars "
        "stay sharp. Only with a Framerate above <recomp-color primary>Original</recomp-color>. <recomp-color primary>Off</recomp-color> keeps "
        "it all sharp.",
        motion_blur_options,
        gt::MotionBlur::On
    );
}

gt::MotionBlur gt::get_motion_blur() {
    return get_graphics_config_enum_value<gt::MotionBlur>(gt::configkeys::graphics::motion_blur);
}

gt::DepthOfField gt::get_depth_of_field() {
    return get_graphics_config_enum_value<gt::DepthOfField>(gt::configkeys::graphics::depth_of_field);
}

gt::HdWheels gt::get_hd_wheels() {
    return get_graphics_config_enum_value<gt::HdWheels>(gt::configkeys::graphics::hd_wheels);
}

gt::DrawDistance gt::get_draw_distance() {
    return get_graphics_config_enum_value<gt::DrawDistance>(gt::configkeys::graphics::draw_distance);
}

gt::FrameRate gt::get_frame_rate() {
    return get_graphics_config_enum_value<gt::FrameRate>(gt::configkeys::graphics::frame_rate);
}

// Gamepad defaults for racing (only used until the player saves their own bindings). GT64's N64 controls (manual):
// A accelerate, B brake, R shift up, L or Z shift down, C buttons camera/rear view, stick or D-pad steering.
static void set_control_defaults() {
    using namespace recompinput;
    auto trigger = [](SDL_GameControllerAxis axis) { return InputField::controller_analog(axis, true); };
    auto button = [](SDL_GameControllerButton b) { return InputField::controller_digital(b); };
    set_default_mapping_for_controller(GameInput::A, { trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT), button(SDL_CONTROLLER_BUTTON_SOUTH) });
    set_default_mapping_for_controller(GameInput::B, { trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT), button(SDL_CONTROLLER_BUTTON_WEST) });
    set_default_mapping_for_controller(GameInput::R, { button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) });
    set_default_mapping_for_controller(GameInput::Z, { button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) });
    set_default_mapping_for_controller(GameInput::L, { button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) });
    // The right shoulder shifts up, so keep it off C-down.
    set_default_mapping_for_controller(GameInput::C_DOWN, { InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTY, true) });

    // Keyboard: the arrows drive the D-pad (the game steers and navigates menus with it), X accelerates, Z brakes,
    // E/Q shift, I/J/K/L are the C buttons.
    auto key = [](SDL_Scancode k) { return InputField::keyboard(k); };
    set_default_mapping_for_keyboard(GameInput::DPAD_LEFT, { key(SDL_SCANCODE_LEFT) });
    set_default_mapping_for_keyboard(GameInput::DPAD_RIGHT, { key(SDL_SCANCODE_RIGHT) });
    set_default_mapping_for_keyboard(GameInput::DPAD_UP, { key(SDL_SCANCODE_UP) });
    set_default_mapping_for_keyboard(GameInput::DPAD_DOWN, { key(SDL_SCANCODE_DOWN) });
    set_default_mapping_for_keyboard(GameInput::A, { key(SDL_SCANCODE_X), key(SDL_SCANCODE_SPACE) });
    set_default_mapping_for_keyboard(GameInput::B, { key(SDL_SCANCODE_Z), key(SDL_SCANCODE_LSHIFT) });
    set_default_mapping_for_keyboard(GameInput::R, { key(SDL_SCANCODE_E) });
    set_default_mapping_for_keyboard(GameInput::Z, { key(SDL_SCANCODE_Q) });
    set_default_mapping_for_keyboard(GameInput::L, { key(SDL_SCANCODE_Q) });
    set_default_mapping_for_keyboard(GameInput::C_LEFT, { key(SDL_SCANCODE_J) });
    set_default_mapping_for_keyboard(GameInput::C_RIGHT, { key(SDL_SCANCODE_L) });
    set_default_mapping_for_keyboard(GameInput::C_UP, { key(SDL_SCANCODE_I) });
    set_default_mapping_for_keyboard(GameInput::C_DOWN, { key(SDL_SCANCODE_K) });
}

static void set_control_descriptions() {
    using namespace recompinput;
    set_game_input_description(GameInput::A, "Accelerate. Confirm in menus.");
    set_game_input_description(GameInput::B, "Brake. Back in menus.");
    set_game_input_description(GameInput::R, "Shift up (manual transmission).");
    set_game_input_description(GameInput::Z, "Shift down (manual transmission).");
    set_game_input_description(GameInput::L, "Shift down (manual transmission).");
    set_game_input_description(GameInput::START, "Pause.");
}

// RT64's "Display" refresh rate draws extra frames by interpolating between the game's frames (made to work with GT64's
// several tasks per frame by patches/deps/rt64-frame-interpolation.patch).
void gt::init_config() {
    std::filesystem::path recomp_dir = recompui::file::get_app_folder_path();
    if (!recomp_dir.empty()) {
        std::filesystem::create_directories(recomp_dir);
    }

    recompui::config::GeneralTabOptions general_options{};
    auto &general_config = recompui::config::create_general_tab(general_options);
    add_general_options(general_config);

    auto &graphics_config = recompui::config::create_graphics_tab();
    add_graphics_options(graphics_config);
    add_draw_distance_option(graphics_config);
    add_hd_wheels_option(graphics_config);
    add_depth_of_field_option(graphics_config);
    add_motion_blur_option(graphics_config);

    set_control_defaults();
    set_control_descriptions();
    recompui::config::create_controls_tab();
    recompui::config::create_sound_tab();
    recompui::config::create_mods_tab();
    // HUD Placement moves HUDs drawn with RT64's extended GBI origins; GT64 has none (its HUD keeps the 4:3 layout), and
    // the only users are the sky's edge anchors, which must reach the window's edges: fixed to Full and hidden.
    recompui::config::get_graphics_config().update_option_hidden(recompui::config::graphics::options::hr_option, true);
    recompui::config::finalize();
    {
        auto &graphics = recompui::config::get_graphics_config();
        bool changed = false;
        const uint32_t full = static_cast<uint32_t>(ultramodern::renderer::HUDRatioMode::Full);
        if (std::get<uint32_t>(graphics.get_option_value(recompui::config::graphics::options::hr_option)) != full) {
            graphics.set_option_value(recompui::config::graphics::options::hr_option, full);
            changed = true;
        }
        if (changed) {
            graphics.save_config();
        }
    }
}
