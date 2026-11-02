#ifndef __GT_GAME_H__
#define __GT_GAME_H__

#include <cstddef>
#include <cstdint>
#include <string>

namespace gt {
    inline const std::string program_name = "GT 64 Recompiled";
    inline const std::u8string program_id = u8"GT64Recompiled";

    void register_overlays();
    void stats_on_vi();
    void stats_on_audio(size_t sample_count);
    void stats_set_rdram(uint8_t* rdram);
    // Game RAM, once the renderer has been created (nullptr before).
    uint8_t* game_rdram();
}

#endif
