#ifndef __GT_CONFIG_H__
#define __GT_CONFIG_H__

#include <string>

namespace gt {
    namespace configkeys {
        namespace graphics {
            inline const std::string frame_rate = "frame_rate";
        }
        namespace general {
            inline const std::string analog_triggers = "analog_triggers";
        }
    }

    enum class FrameRate {
        Original, // the game's own frame rate, like the N64
        Fps60,
    };

    enum class AnalogTriggers {
        Off, // the N64's digital A and B
        On,  // throttle and brake follow how far the buttons bound to A and B are pressed
    };

    void init_config();
    FrameRate get_frame_rate();
    AnalogTriggers get_analog_triggers();
}

#endif
