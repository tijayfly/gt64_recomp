// Native helpers called from recompiler hooks (gt64.us.toml) to implement enhancements.
#include <cstdint>

#include "gt_config.h"

extern "C" int gt_fps60_enabled() {
    return gt::get_frame_rate() == gt::FrameRate::Fps60;
}
