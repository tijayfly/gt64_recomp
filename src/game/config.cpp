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

void gt::init_config() {
    std::filesystem::path recomp_dir = recompui::file::get_app_folder_path();
    if (!recomp_dir.empty()) {
        std::filesystem::create_directories(recomp_dir);
    }

    recompui::config::GeneralTabOptions general_options{};
    recompui::config::create_general_tab(general_options);

    auto &graphics_config = recompui::config::create_graphics_tab();
    add_graphics_options(graphics_config);

    set_control_defaults();
    set_control_descriptions();
    recompui::config::create_controls_tab();
    recompui::config::create_sound_tab();
    recompui::config::create_mods_tab();
    recompui::config::finalize();
}
