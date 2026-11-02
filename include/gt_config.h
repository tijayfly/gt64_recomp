#ifndef __GT_CONFIG_H__
#define __GT_CONFIG_H__

#include <string>

namespace gt {
    namespace configkeys {
        namespace graphics {
            inline const std::string frame_rate = "frame_rate";
        }
    }

    enum class FrameRate {
        Original, // the game's own frame rate, like the N64
        Fps60,
    };

    void init_config();
    FrameRate get_frame_rate();
}

#endif
