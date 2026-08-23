#include <unordered_map>
#include <utility>

#include "../basic/mahjong.h"
#include "../basic/mahjong_utils.h"
#include "qingque.h"

namespace qingque {

    using namespace mahjong;

   std::unordered_map<tile_t, std::unordered_map<tile_t, std::pair<double, double>>> get_all_waits(const w_data& data, const hand& h) {
        std::unordered_map<tile_t, std::unordered_map<tile_t, std::pair<double, double>>> all_waits;
        std::vector<tile_t> tiles = h.counter(false).tiles();
        int pair_count = 0;
        for (tile_t ti : tile_set::all_tiles) {
            pair_count += h.counter(false).count(ti) / 2;
        }
        if (pair_count >= 5) goto a;
        for (tile_t ti : tile_set::all_tiles) {
            if (h.counter().count(ti) >= 4) continue;
            std::vector<tile_t> new_tiles = tiles;
            new_tiles.push_back(ti);
            hand new_hand(new_tiles, h.melds(), static_cast<tile_t>(h.winning_tile()), h.winning_type(), true, false);
            if (new_hand.decompose().size() || is_thirteen_orphans(new_hand)) goto a;
        }
        return all_waits;
a:      auto base_counter = h.counter(false);
        for (tile_t to_be_replaced : base_counter.tiles(false)) {
            for (tile_t to_be_added : tile_set::all_tiles) {
                if (base_counter.count(to_be_added) >= 4) continue;
                auto new_counter = base_counter - to_be_replaced + to_be_added;
                hand new_hand(new_counter.tiles(), h.melds(), to_be_added, 0, true, false);
                new_hand.set_winning_type(win_type(false, false, false, false, h.winning_type().seat_wind()));
                if (!new_hand.is_valid()) continue;
                if (!is_winning_hand(new_hand)) continue;
                auto [fan, res] = get_fan(data, new_hand);
                new_hand.set_winning_type(win_type(true, false, false, false, h.winning_type().seat_wind()));
                auto [fan2, res2] = get_fan(data, new_hand);
                if (!all_waits.contains(to_be_replaced)) {
                    all_waits[to_be_replaced] = {};
                }
                all_waits[to_be_replaced][to_be_added] = {fan, fan2};
            }
        }
        return all_waits;
    }

}
