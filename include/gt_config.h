#ifndef __GT_CONFIG_H__
#define __GT_CONFIG_H__

#include <string>

namespace gt {
    namespace configkeys {
        namespace graphics {
            inline const std::string frame_rate = "frame_rate";
            inline const std::string draw_distance = "draw_distance";
            inline const std::string hd_wheels = "hd_wheels";
            inline const std::string depth_of_field = "depth_of_field";
            inline const std::string motion_blur = "motion_blur";
        }
        namespace general {
            inline const std::string analog_triggers = "analog_triggers";
            inline const std::string auto_gearbox = "auto_gearbox";
            inline const std::string slip_rumble = "slip_rumble";
        }
    }

    enum class FrameRate {
        Original, // the game's own frame rate, like the N64
        Fps60,
    };

    enum class DrawDistance {
        Original, // the game's: its track sections near the camera, simpler models and nothing past about 5700 units
        Extended, // 20 sections ahead and 10 behind, detailed models at any distance, the far plane moved out
        Maximum,  // the whole track (Extended's counts while more than one race view is drawn a frame)
    };

    enum class AnalogTriggers {
        Off, // the N64's digital A and B
        On,  // throttle and brake follow how far the buttons bound to A and B are pressed
    };

    enum class AutoGearbox {
        Original, // the game's shift points (per car)
        Custom,   // up at 90% of the 10000 RPM rev limit, down at 55% (never into neutral or reverse)
    };

    enum class SlipRumble {
        Off,
        On, // rumble while the car's wheels spin or it slides, harder the more it slips
    };

    enum class HdWheels {
        Off, // the game's six-sided wheels
        On,  // modelled wheels (round tyres, five-spoke rims) in their place
    };

    enum class DepthOfField {
        Off,
        On, // blur the distance past the original draw distance's reach (Extended and Maximum only)
    };

    enum class MotionBlur {
        Off,
        On, // blur the race view along the camera's motion (the cars stay sharp)
    };

    void init_config();
    FrameRate get_frame_rate();
    AnalogTriggers get_analog_triggers();
    AutoGearbox get_auto_gearbox();
    SlipRumble get_slip_rumble();
    DrawDistance get_draw_distance();
    HdWheels get_hd_wheels();
    DepthOfField get_depth_of_field();
    MotionBlur get_motion_blur();
}

#endif
