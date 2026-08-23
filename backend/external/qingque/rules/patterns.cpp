#include "../basic/mahjong.h"
#include "../basic/mahjong_utils.h"
#include "qingque.h"

namespace qingque {

    using namespace mahjong;

    namespace patterns {

        unsigned long long nine_gates_m_s = 0b011001001001001001001001011000ull;
        unsigned long long nine_gates_p = 0b011001001001001001001001011000ull << 32;

        using namespace tile_literals;
        using enum meld_type;

        const std::array<std::array<meld_t, 3>, 42> mixed_shifted_triplets = {{
            {triplet | 1_m, triplet | 2_p, triplet | 3_s}, {triplet | 2_m, triplet | 3_p, triplet | 4_s}, {triplet | 3_m, triplet | 4_p, triplet | 5_s}, {triplet | 4_m, triplet | 5_p, triplet | 6_s},
            {triplet | 5_m, triplet | 6_p, triplet | 7_s}, {triplet | 6_m, triplet | 7_p, triplet | 8_s}, {triplet | 7_m, triplet | 8_p, triplet | 9_s},
            {triplet | 1_p, triplet | 2_s, triplet | 3_m}, {triplet | 2_p, triplet | 3_s, triplet | 4_m}, {triplet | 3_p, triplet | 4_s, triplet | 5_m}, {triplet | 4_p, triplet | 5_s, triplet | 6_m},
            {triplet | 5_p, triplet | 6_s, triplet | 7_m}, {triplet | 6_p, triplet | 7_s, triplet | 8_m}, {triplet | 7_p, triplet | 8_s, triplet | 9_m},
            {triplet | 1_s, triplet | 2_m, triplet | 3_p}, {triplet | 2_s, triplet | 3_m, triplet | 4_p}, {triplet | 3_s, triplet | 4_m, triplet | 5_p}, {triplet | 4_s, triplet | 5_m, triplet | 6_p},
            {triplet | 5_s, triplet | 6_m, triplet | 7_p}, {triplet | 6_s, triplet | 7_m, triplet | 8_p}, {triplet | 7_s, triplet | 8_m, triplet | 9_p},
            {triplet | 1_m, triplet | 2_s, triplet | 3_p}, {triplet | 2_m, triplet | 3_s, triplet | 4_p}, {triplet | 3_m, triplet | 4_s, triplet | 5_p}, {triplet | 4_m, triplet | 5_s, triplet | 6_p},
            {triplet | 5_m, triplet | 6_s, triplet | 7_p}, {triplet | 6_m, triplet | 7_s, triplet | 8_p}, {triplet | 7_m, triplet | 8_s, triplet | 9_p},
            {triplet | 1_p, triplet | 2_m, triplet | 3_s}, {triplet | 2_p, triplet | 3_m, triplet | 4_s}, {triplet | 3_p, triplet | 4_m, triplet | 5_s}, {triplet | 4_p, triplet | 5_m, triplet | 6_s},
            {triplet | 5_p, triplet | 6_m, triplet | 7_s}, {triplet | 6_p, triplet | 7_m, triplet | 8_s}, {triplet | 7_p, triplet | 8_m, triplet | 9_s},
            {triplet | 1_s, triplet | 2_p, triplet | 3_m}, {triplet | 2_s, triplet | 3_p, triplet | 4_m}, {triplet | 3_s, triplet | 4_p, triplet | 5_m}, {triplet | 4_s, triplet | 5_p, triplet | 6_m},
            {triplet | 5_s, triplet | 6_p, triplet | 7_m}, {triplet | 6_s, triplet | 7_p, triplet | 8_m}, {triplet | 7_s, triplet | 8_p, triplet | 9_m}
        }};

        const std::array<std::array<meld_t, 3>, 30> mixed_shifted_sequences = {{
            {2_m, 3_p, 4_s}, {3_m, 4_p, 5_s}, {4_m, 5_p, 6_s}, {5_m, 6_p, 7_s}, {6_m, 7_p, 8_s},
            {2_p, 3_s, 4_m}, {3_p, 4_s, 5_m}, {4_p, 5_s, 6_m}, {5_p, 6_s, 7_m}, {6_p, 7_s, 8_m},
            {2_s, 3_m, 4_p}, {3_s, 4_m, 5_p}, {4_s, 5_m, 6_p}, {5_s, 6_m, 7_p}, {6_s, 7_m, 8_p},
            {2_m, 3_s, 4_p}, {3_m, 4_s, 5_p}, {4_m, 5_s, 6_p}, {5_m, 6_s, 7_p}, {6_m, 7_s, 8_p},
            {2_p, 3_m, 4_s}, {3_p, 4_m, 5_s}, {4_p, 5_m, 6_s}, {5_p, 6_m, 7_s}, {6_p, 7_m, 8_s},
            {2_s, 3_p, 4_m}, {3_s, 4_p, 5_m}, {4_s, 5_p, 6_m}, {5_s, 6_p, 7_m}, {6_s, 7_p, 8_m}
        }};

        const std::array<std::array<meld_t, 3>, 18> mixed_chained_sequences = {{
            {2_m, 4_p, 6_s}, {3_m, 5_p, 7_s}, {4_m, 6_p, 8_s},
            {2_p, 4_s, 6_m}, {3_p, 5_s, 7_m}, {4_p, 6_s, 8_m},
            {2_s, 4_m, 6_p}, {3_s, 5_m, 7_p}, {4_s, 6_m, 8_p},
            {2_m, 4_s, 6_p}, {3_m, 5_s, 7_p}, {4_m, 6_s, 8_p},
            {2_p, 4_m, 6_s}, {3_p, 5_m, 7_s}, {4_p, 6_m, 8_s},
            {2_s, 4_p, 6_m}, {3_s, 5_p, 7_m}, {4_s, 6_p, 8_m}
        }};

        const std::array<std::array<meld_t, 3>, 6> mixed_straight = {{
            {2_m, 5_p, 8_s}, {2_p, 5_s, 8_m}, {2_s, 5_m, 8_p},
            {2_m, 5_s, 8_p}, {2_p, 5_m, 8_s}, {2_s, 5_p, 8_m}
        }};

        const std::array<std::array<meld_t, 4>, 15> mirrored_short_straights = {{
            {2_m, 5_m, 2_p, 5_p}, {3_m, 6_m, 3_p, 6_p}, {4_m, 7_m, 4_p, 7_p}, {5_m, 8_m, 5_p, 8_p}, {2_m, 8_m, 2_p, 8_p},
            {2_p, 5_p, 2_s, 5_s}, {3_p, 6_p, 3_s, 6_s}, {4_p, 7_p, 4_s, 7_s}, {5_p, 8_p, 5_s, 8_s}, {2_p, 8_p, 2_s, 8_s},
            {2_s, 5_s, 2_m, 5_m}, {3_s, 6_s, 3_m, 6_m}, {4_s, 7_s, 4_m, 7_m}, {5_s, 8_s, 5_m, 8_m}, {2_s, 8_s, 2_m, 8_m}
        }};

        unsigned long long knitted_tiles = 0b001000000001000000001ull;
        unsigned long long honours = 0b001001001001001001001000ull << 32;

        const std::array<tile_counter, 6> honours_and_knitted_tiles = {
            tile_counter((knitted_tiles << 3) + (knitted_tiles << (32 + 6)), honours + (knitted_tiles << 9)),
            tile_counter((knitted_tiles << 6) + (knitted_tiles << (32 + 9)), honours + (knitted_tiles << 3)),
            tile_counter((knitted_tiles << 9) + (knitted_tiles << (32 + 3)), honours + (knitted_tiles << 6)),
            tile_counter((knitted_tiles << 3) + (knitted_tiles << (32 + 9)), honours + (knitted_tiles << 6)),
            tile_counter((knitted_tiles << 6) + (knitted_tiles << (32 + 3)), honours + (knitted_tiles << 9)),
            tile_counter((knitted_tiles << 9) + (knitted_tiles << (32 + 6)), honours + (knitted_tiles << 3))
        };

    }

}
