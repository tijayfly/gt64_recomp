// Fog on the cars. The game never fogs them (their display lists draw in one cycle, without the fog blender): it only
// drew them close enough for that not to show. With an extended draw distance, cars far down the track stood out of the
// fog that hides the road under them, as if flying. While it's on, the cars' display lists (the parts inside their
// matrix groups, see gt_part_begin) are patched as each graphics task is submitted to blend in the fog the way the
// track does: two cycles, the first mixing in the fog color by the shade alpha the fog sets, the second the original
// blender; the combiner's second cycle passes the first's result through. Undone when the draw distance is Original.
#include <cstdint>
#include <unordered_map>

#include "gt_config.h"

namespace {
    uint8_t* ram = nullptr;
    uint32_t rd32(uint32_t a) { return *reinterpret_cast<uint32_t*>(ram + (a & 0x7FFFFFu)); }
    void wr32(uint32_t a, uint32_t v) { *reinterpret_cast<uint32_t*>(ram + (a & 0x7FFFFFu)) = v; }

    struct Change {
        uint32_t original, patched;
    };
    std::unordered_map<uint32_t, Change> changes;   // by the address of the word changed

    void set(uint32_t a, uint32_t v) {
        const uint32_t now = rd32(a);
        if (now == v) return;
        auto it = changes.find(a);
        if (it == changes.end() || it->second.patched != now) {
            changes[a] = Change{ now, v };
        } else {
            it->second.patched = v;
        }
        wr32(a, v);
    }

    void undo_all() {
        for (const auto& [a, c] : changes) {
            if (rd32(a) == c.patched) wr32(a, c.original);
        }
        changes.clear();
    }

    // A combiner whose first cycle doesn't read the shade alpha (the fog takes it over).
    bool alpha_free_of_shade(uint32_t w0, uint32_t w1) {
        const uint32_t aa = (w0 >> 12) & 7, ac = (w0 >> 9) & 7, ab = (w1 >> 12) & 7, ad = (w1 >> 9) & 7;
        return aa != 4 && ab != 4 && ac != 4 && ad != 4;
    }

    void patch(uint32_t a, uint32_t w0, uint32_t w1) {
        const uint32_t op = w0 >> 24;
        if (op == 0xBA && w0 == 0xBA001402u && w1 == 0u) {
            set(a + 4, 0x00100000u);                                // cycle type: 2 cycles
        } else if (op == 0xB9 && w0 == 0xB900031Du) {
            // Render mode: first cycle IN * A_SHADE + FOG * (1 - A_SHADE), as the track's; the second the original's.
            set(a + 4, (w1 & ~0xCCCC0000u) | 0x08C00000u);
        } else if (op == 0xB7) {
            set(a + 4, w1 | 0x00010000u);                           // G_FOG
        } else if (op == 0xB6 && (w1 & 0x00010000u)) {
            set(a + 4, w1 & ~0x00010000u);
        } else if (op == 0xFC && alpha_free_of_shade(w0, w1)) {
            // Second cycle: (0 - 0) * 0 + COMBINED for both color and alpha.
            const uint32_t n0 = (w0 & ~0x000001FFu & ~(0xFu << 5)) | (0xFu << 5) | 0x1Fu;
            const uint32_t n1 = (w1 & ~((0xFu << 24) | (7u << 21) | (7u << 18) | (7u << 6) | (7u << 3) | 7u)) |
                (0xFu << 24) | (7u << 21) | (7u << 18) | (0u << 6) | (7u << 3) | 0u;
            set(a, n0);
            set(a + 4, n1);
        }
    }

    void walk(uint32_t dl, uint32_t* segs, int depth, bool car, int& budget) {
        uint32_t half = 0;
        for (int i = 0; budget > 0; i++, budget--) {
            const uint32_t a = dl + i * 8;
            if ((a & 0x7FFFFFu) + 8 > 0x800000u) return;
            const uint32_t w0 = rd32(a), w1 = rd32(a + 4);
            const uint32_t op = w0 >> 24;
            if (op == 0xB8) return;
            if (w0 == 0x6400000Cu) {
                car = true;       // a car part's matrix group
            } else if (w0 == 0x6400000Du) {
                car = false;
            } else if (op == 0xBC && (w0 & 0xFF) == 6) {
                segs[((w0 >> 8) & 0xFFFF) / 4 & 15] = w1 & 0x00FFFFFFu;
            } else if (op == 0xB4) {
                half = w1;        // G_RDPHALF_1: G_BRANCH_Z's list
            } else if (op == 0xB0) {
                if (car && depth < 6) {
                    walk(0x80000000u | ((segs[(half >> 24) & 15] + (half & 0xFFFFFF)) & 0x7FFFFFu), segs, depth + 1, car, budget);
                }
            } else if (op == 0x06) {
                const uint32_t target = 0x80000000u | ((segs[(w1 >> 24) & 15] + (w1 & 0xFFFFFF)) & 0x7FFFFFu);
                if (depth < 6) {
                    walk(target, segs, depth + 1, car, budget);
                }
                if ((w0 >> 16) & 0xFF) return;  // branch
            } else if (car) {
                patch(a, w0, w1);
            }
        }
    }
}

extern "C" void gt_car_fog(uint8_t* rdram, uint32_t task) {
    ram = rdram;
    if (gt::get_draw_distance() == gt::DrawDistance::Original) {
        if (!changes.empty()) undo_all();
        return;
    }
    if (rd32(task) != 1) return;  // M_GFXTASK
    if (changes.size() > 100000) {
        undo_all();
    }
    uint32_t segs[16] = {};
    int budget = 60000;
    walk(rd32(task + 0x30), segs, 0, false, budget);
}
