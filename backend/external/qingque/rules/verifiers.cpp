#include "../basic/mahjong.h"
#include "../basic/mahjong_utils.h"
#include "qingque.h"

namespace qingque {

    using namespace mahjong;

    verifier is_seven_pairs([](const hand& h) {
        if (h.melds().size()) return false;
        for (tile_t ti : tile_set::all_tiles)
            if (h.counter().count(ti) & 1) return false;
        return true;
    });

    verifier is_thirteen_orphans([](const hand& h) {
        if (h.melds().size()) return false;
        using namespace tile_literals;
        using namespace honours;
        for (tile_t ti : {1_m, 9_m, 1_p, 9_p, 1_s, 9_s, E, S, W, N, C, F, P})
            if (!h.counter().count(ti)) return false;
        return h.counter().count({1_m, 9_m, 1_p, 9_p, 1_s, 9_s, E, S, W, N, C, F, P}) >= 14;
    });

    verifier input_verifier([](const hand& h) {
        if (!h.is_valid()) return false;
        uint8_t kong_count = 0;
        for (const auto& m : h.melds())
            if (m.type() == meld_type::kong) ++kong_count;
        if (kong_count == 0 && h.winning_type()(win_type::kong_related | win_type::self_drawn)) return false;
        if (h.melds().size() && h.winning_type()(win_type::heavenly_or_earthly_hand)) return false;
        if (h.counter().count(h.winning_tile()) > 1 && h.winning_type()(win_type::kong_related, win_type::self_drawn)) return false;
        if (h.winning_type()(win_type::final_tile | win_type::heavenly_or_earthly_hand)) return false;
        return true;
    });

    verifier is_winning_hand([](const hand& h) {
        if (!input_verifier(h)) return false;
        if (h.decompose().size()) return true;
        if (is_seven_pairs(h)) return true;
        if (is_thirteen_orphans(h)) return true;
        return false;
    });

}
