#include <filesystem>

#include "recompui/config.h"
#include "recompinput/input_mapping.h"
#include "util/file.h"

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
        "<recomp-color primary>60 FPS</recomp-color> renders every video frame. "
        "<recomp-color primary>Original</recomp-color> matches the N64.",
        frame_rate_options,
        gt::FrameRate::Fps60
    );
}

template <typename T = uint32_t>
T get_graphics_config_enum_value(const std::string& option_id) {
    return static_cast<T>(std::get<uint32_t>(recompui::config::get_graphics_config().get_option_value(option_id)));
}

gt::FrameRate gt::get_frame_rate() {
    return get_graphics_config_enum_value<gt::FrameRate>(gt::configkeys::graphics::frame_rate);
}

// Racing defaults: triggers for throttle and brake are mapped in the controls tab descriptions; the N64 layout is
// A = accelerate, B = brake, so the frontend's defaults (A/B on the face buttons) already work on a gamepad.
static void set_control_descriptions() {
    using namespace recompinput;
    set_game_input_description(GameInput::A, "Accelerate. Confirm in menus.");
    set_game_input_description(GameInput::B, "Brake / reverse. Back in menus.");
    set_game_input_description(GameInput::START, "Pause.");
}

void gt::init_config() {
    std::filesystem::path recomp_dir = recompui::file::get_app_folder_path();
    if (!recomp_dir.empty()) {
        std::filesystem::create_directories(recomp_dir);
    }

    recompui::config::GeneralTabOptions general_options{};
    recompui::config::create_general_tab(general_options);

    auto &graphics_config = recompui::config::create_graphics_tab();
    add_graphics_options(graphics_config);

    set_control_descriptions();
    recompui::config::create_controls_tab();
    recompui::config::create_sound_tab();
    recompui::config::create_mods_tab();
    recompui::config::finalize();
}
