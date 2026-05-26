#include <math.h>
#include <iostream>
#include <unordered_map>

#include "../basic/mahjong.h"
#include "../basic/mahjong_utils.h"
#include "qingque_data.h"
#include "qingque.h"

namespace qingque {

    using namespace mahjong;

    namespace patterns {

        unsigned long long nine_gates_m_s = 0b011001001001001001001001011000ull;
        unsigned long long nine_gates_p = 0b011001001001001001001001011000ull << 32;

        using namespace tile_literals;
        using enum meld_type;

        std::initializer_list<std::initializer_list<meld_t>> mixed_shifted_triplets = {
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
        };

        std::initializer_list<std::initializer_list<meld_t>> mixed_shifted_sequences = {
            {2_m, 3_p, 4_s}, {3_m, 4_p, 5_s}, {4_m, 5_p, 6_s}, {5_m, 6_p, 7_s}, {6_m, 7_p, 8_s},
            {2_p, 3_s, 4_m}, {3_p, 4_s, 5_m}, {4_p, 5_s, 6_m}, {5_p, 6_s, 7_m}, {6_p, 7_s, 8_m},
            {2_s, 3_m, 4_p}, {3_s, 4_m, 5_p}, {4_s, 5_m, 6_p}, {5_s, 6_m, 7_p}, {6_s, 7_m, 8_p},
            {2_m, 3_s, 4_p}, {3_m, 4_s, 5_p}, {4_m, 5_s, 6_p}, {5_m, 6_s, 7_p}, {6_m, 7_s, 8_p},
            {2_p, 3_m, 4_s}, {3_p, 4_m, 5_s}, {4_p, 5_m, 6_s}, {5_p, 6_m, 7_s}, {6_p, 7_m, 8_s},
            {2_s, 3_p, 4_m}, {3_s, 4_p, 5_m}, {4_s, 5_p, 6_m}, {5_s, 6_p, 7_m}, {6_s, 7_p, 8_m}
        };

        std::initializer_list<std::initializer_list<meld_t>> mixed_chained_sequences = {
            {2_m, 4_p, 6_s}, {3_m, 5_p, 7_s}, {4_m, 6_p, 8_s},
            {2_p, 4_s, 6_m}, {3_p, 5_s, 7_m}, {4_p, 6_s, 8_m},
            {2_s, 4_m, 6_p}, {3_s, 5_m, 7_p}, {4_s, 6_m, 8_p},
            {2_m, 4_s, 6_p}, {3_m, 5_s, 7_p}, {4_m, 6_s, 8_p},
            {2_p, 4_m, 6_s}, {3_p, 5_m, 7_s}, {4_p, 6_m, 8_s},
            {2_s, 4_p, 6_m}, {3_s, 5_p, 7_m}, {4_s, 6_p, 8_m}
        };

        std::initializer_list<std::initializer_list<meld_t>> mixed_straight = {
            {2_m, 5_p, 8_s}, {2_p, 5_s, 8_m}, {2_s, 5_m, 8_p},
            {2_m, 5_s, 8_p}, {2_p, 5_m, 8_s}, {2_s, 5_p, 8_m}
        };

        std::initializer_list<std::initializer_list<meld_t>> mirrored_short_straights = {
            {2_m, 5_m, 2_p, 5_p}, {3_m, 6_m, 3_p, 6_p}, {4_m, 7_m, 4_p, 7_p}, {5_m, 8_m, 5_p, 8_p}, {2_m, 8_m, 2_p, 8_p},
            {2_p, 5_p, 2_s, 5_s}, {3_p, 6_p, 3_s, 6_s}, {4_p, 7_p, 4_s, 7_s}, {5_p, 8_p, 5_s, 8_s}, {2_p, 8_p, 2_s, 8_s},
            {2_s, 5_s, 2_m, 5_m}, {3_s, 6_s, 3_m, 6_m}, {4_s, 7_s, 4_m, 7_m}, {5_s, 8_s, 5_m, 8_m}, {2_s, 8_s, 2_m, 8_m}
        };

        unsigned long long knitted_tiles = 0b001000000001000000001ull;
        unsigned long long honours = 0b001001001001001001001000ull << 32;

        std::initializer_list<tile_counter> honours_and_knitted_tiles = {
            tile_counter((knitted_tiles << 3) + (knitted_tiles << (32 + 6)), honours + (knitted_tiles << 9)),
            tile_counter((knitted_tiles << 6) + (knitted_tiles << (32 + 9)), honours + (knitted_tiles << 3)),
            tile_counter((knitted_tiles << 9) + (knitted_tiles << (32 + 3)), honours + (knitted_tiles << 6)),
            tile_counter((knitted_tiles << 3) + (knitted_tiles << (32 + 9)), honours + (knitted_tiles << 6)),
            tile_counter((knitted_tiles << 6) + (knitted_tiles << (32 + 3)), honours + (knitted_tiles << 9)),
            tile_counter((knitted_tiles << 9) + (knitted_tiles << (32 + 6)), honours + (knitted_tiles << 3))
        };

    }

    using fan = scoring_element<uint8_t, tag>;

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

    using namespace tile_literals;

    namespace criteria {

        using res_t = uint8_t;
        using res_v = std::vector<uint8_t>;
        inline res_t heavenly_hand(const hand& h) {
            return h.winning_type()(win_type::heavenly_or_earthly_hand | win_type::self_drawn);
        }

        inline res_t earthly_hand(const hand& h) {
            return h.winning_type()(win_type::heavenly_or_earthly_hand, win_type::self_drawn);
        }

        inline res_t out_with_replacement_tile(const hand& h) {
            return h.winning_type()(win_type::kong_related | win_type::self_drawn);
        }

        inline res_t last_tile_draw(const hand& h) {
            return h.winning_type()(win_type::final_tile | win_type::self_drawn);
        }

        inline res_t last_tile_claim(const hand& h) {
            return h.winning_type()(win_type::final_tile, win_type::self_drawn);
        }

        inline res_t robbing_the_kong(const hand& h) {
            return h.winning_type()(win_type::kong_related, win_type::self_drawn);
        }

        inline res_t self_drawn(const hand& h) {
            return h.winning_type()(win_type::self_drawn);
        }

        inline res_t concealed_hand(const hand& h) {
            for (const auto& m : h.melds())
                if (!m.concealed()) return false;
            return true;
        }

        inline res_v four_concealed_kongs(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (const meld& m : d.melds())
                    if (!m.concealed() || m.type() != meld_type::kong) return false;
                return true;
            });
        }

        inline res_v three_concealed_kongs(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t counter = 0;
                for (const meld& m : d.melds())
                    if (m.concealed() && m.type() == meld_type::kong) ++counter;
                return counter >= 3;
            });
        }

        inline res_v two_concealed_kongs(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t counter = 0;
                for (const meld& m : d.melds())
                    if (m.concealed() && m.type() == meld_type::kong) ++counter;
                return counter >= 2;
            });
        }

        inline res_v concealed_kong(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t counter = 0;
                for (const meld& m : d.melds())
                    if (m.concealed() && m.type() == meld_type::kong) ++counter;
                return counter >= 1;
            });
        }

        inline res_t four_kongs(const hand& h) {
            return h.counter().count() >= 18;
        }

        inline res_t three_kongs(const hand& h) {
            return h.counter().count() >= 17;
        }

        inline res_t two_kongs(const hand& h) {
            return h.counter().count() >= 16;
        }

        inline res_t kong(const hand& h) {
            return h.counter().count() >= 15;
        }

        inline res_v four_concealed_triplets(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (const meld& m : d.melds())
                    if (!m.concealed() || m.type() == meld_type::sequence) return false;
                return true;
            });
        }

        inline res_v three_concealed_triplets(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t counter = 0;
                for (const meld& m : d.melds())
                    if (m.concealed() && m.type() > meld_type::sequence) ++counter;
                return counter >= 3;
            });
        }

        inline res_v two_concealed_triplets(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t counter = 0;
                for (const meld& m : d.melds())
                    if (m.concealed() && m.type() > meld_type::sequence) ++counter;
                return counter >= 2;
            });
        }

        inline res_v concealed_triplet(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t counter = 0;
                for (const meld& m : d.melds())
                    if (m.concealed() && m.type() > meld_type::sequence) ++counter;
                return counter >= 1;
            });
        }

        inline res_v all_triplets(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (const meld& m : d.melds())
                    if (m.type() == meld_type::sequence) return false;
                return true;
            });
        }

        inline res_t all_honours(const hand& h) {
            for (tile_t ti : tile_set::numbered_tiles)
            if (h.counter().count(ti)) return false;
            return true;
        }

        inline res_t big_four_winds(const hand& h) {
            for (tile_t ti : tile_set::wind_tiles)
            if (h.counter().count(ti) < 3) return false;
            return true;
        }

        inline res_t little_four_winds(const hand& h) {
            uint8_t count = 0u;
            for (tile_t ti : tile_set::wind_tiles)
            count += (h.counter().count(ti) >= 3) + (h.counter().count(ti) >= 2);
            return count == 7;
        }

        inline res_t big_three_dragons(const hand& h) {
            for (tile_t ti : tile_set::dragon_tiles)
            if (h.counter().count(ti) < 3) return false;
            return true;
        }

        inline res_t little_three_dragons(const hand& h) {
            uint8_t count = 0u;
            for (tile_t ti : tile_set::dragon_tiles)
            count += (h.counter().count(ti) >= 3) + (h.counter().count(ti) >= 2);
            return count == 5;
        }

        inline res_t fan_tile_1p(const hand& h) {
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            for (tile_t ti : fan_tiles)
                if (h.counter().count(ti) >= 2) return true;
            return false;
        }

        inline res_v fan_tile_2p(const hand& h) {
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            return utils::for_all_decompositions<res_t>(h, [&fan_tiles](const hand::decomposition& d) {
                uint8_t cnt = 0;
                for (tile_t ti : fan_tiles)
                    cnt += (d.counter().count(ti) >= 2);
                return cnt >= 2;
            }, [&fan_tiles](const hand& h) {
                return utils::count_pair_of(h, fan_tiles) >= 2;
            });
        }

        inline res_v fan_tile_3p(const hand& h) {
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            return utils::for_all_decompositions<res_t>(h, [&fan_tiles](const hand::decomposition& d) {
                uint8_t cnt = 0;
                for (tile_t ti : fan_tiles)
                    cnt += (d.counter().count(ti) >= 2);
                return cnt >= 3;
            }, [&fan_tiles](const hand& h) {
                return utils::count_pair_of(h, fan_tiles) >= 3;
            });
        }

        inline res_v fan_tile_4p(const hand& h) {
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            return utils::for_all_decompositions<res_t>(h, [&fan_tiles](const hand::decomposition& d) {
                uint8_t cnt = 0;
                for (tile_t ti : fan_tiles)
                    cnt += (d.counter().count(ti) >= 2);
                return cnt >= 4;
            }, [&fan_tiles](const hand& h) {
                return utils::count_pair_of(h, fan_tiles) >= 4;
            });
        }

        inline res_t fan_tile_5p(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            return utils::count_pair_of(h, fan_tiles) >= 5;
        }

        inline res_t fan_tile_6p(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            return utils::count_pair_of(h, fan_tiles) >= 6;
        }

        inline res_t fan_tile_7p(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            return utils::count_pair_of(h, fan_tiles) >= 7;
        }

        inline res_t fan_tile_1t(const hand& h) {
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            uint8_t cnt = 0;
            for (tile_t ti : fan_tiles)
                cnt += (h.counter().count(ti) >= 3);
            return cnt >= 1;
        }

        inline res_t fan_tile_2t(const hand& h) {
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            uint8_t cnt = 0;
            for (tile_t ti : fan_tiles)
                cnt += (h.counter().count(ti) >= 3);
            return cnt >= 2;
        }

        inline res_t fan_tile_3t(const hand& h) {
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            uint8_t cnt = 0;
            for (tile_t ti : fan_tiles)
                cnt += (h.counter().count(ti) >= 3);
            return cnt >= 3;
        }

        inline res_t fan_tile_4t(const hand& h) {
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            uint8_t cnt = 0;
            for (tile_t ti : fan_tiles)
                cnt += (h.counter().count(ti) >= 3);
            return cnt >= 4;
        }

        inline res_t all_terminals(const hand& h) {
            for (tile_t ti : tile_set::all_tiles)
            if (h.counter().count(ti) && tile(ti).num() != 1 && tile(ti).num() != 9) return false;
            return true;
        }

        inline res_t all_terminals_and_honours(const hand& h) {
            for (tile_t ti : tile_set::simple_tiles)
                if (h.counter().count(ti)) return false;
            return true;
        }

        inline res_v pure_outside_hand(const hand& h) {
            auto poh_check = [](const hand::decomposition& d) {
                if (!d.pair().is_in(tile_set::terminal_tiles)) return false;
                for (const meld& m : d.melds())
                    if (!m.contains(tile_set::terminal_tiles)) return false;
                return true;
            };
            return utils::for_all_decompositions<res_t>(h, poh_check, all_terminals);
        }

        inline res_v mixed_outside_hand(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                if (!d.pair().is_in(tile_set::terminal_honour_tiles)) return false;
                for (const meld& m : d.melds())
                    if (!m.contains(tile_set::terminal_honour_tiles)) return false;
                return true;
            }, all_terminals_and_honours);
        }

        inline res_t nine_gates(const hand& h) {
            auto c = h.counter(false);
            c.add(h.winning_tile(), -1);
            return (c == std::make_pair(patterns::nine_gates_m_s, 0ull) || 
                    c == std::make_pair(0ull, patterns::nine_gates_m_s) || 
                    c == std::make_pair(patterns::nine_gates_p, 0ull));
        }

        inline res_t full_flush(const hand& h) {
            for (tile_t ti : tile_set::honour_tiles)
                if (h.counter().count(ti)) return false;
            auto check_suit = [](const hand& h, suit_type st) {
                for (tile_t ti : tile_set::tiles_of_suit(st))
                    if (h.counter().count(ti)) return true;
                return false;
            };
            return (check_suit(h, suit_type::m) + check_suit(h, suit_type::p) + check_suit(h, suit_type::s) == 1);
        }

        inline res_t half_flush(const hand& h) {
            auto check_suit = [](const hand& h, suit_type st) {
                for (tile_t ti : tile_set::tiles_of_suit(st))
                    if (h.counter().count(ti)) return true;
                return false;
            };
            return (check_suit(h, suit_type::m) + check_suit(h, suit_type::p) + check_suit(h, suit_type::s) <= 1);
        }

        inline res_t all_types(const hand& h) {
            if (is_seven_pairs(h) || is_thirteen_orphans(h)) return false;
            if (!h.counter().count(tile_set::character_tiles)) return false;
            if (!h.counter().count(tile_set::bamboo_tiles)) return false;
            if (!h.counter().count(tile_set::dot_tiles)) return false;
            if (!h.counter().count(tile_set::wind_tiles)) return false;
            if (!h.counter().count(tile_set::dragon_tiles)) return false;
            return true;
        }

        inline res_t two_numbers(const hand& h) {
            uint16_t num_table = 0u;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti)) {
                    if (tile(ti).num() == 0) return false;
                    num_table |= 1 << tile(ti).num();
                }
            return utils::popcount(num_table) == 2u;
        }

        inline res_t two_consecutive_numbers(const hand& h) {
            uint16_t num_table = 0u;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti))
                    num_table |= 1 << tile(ti).num();
            for (uint16_t mask = 0b110u; mask <= 0b1100000000u; mask <<= 1U)
                if ((num_table | mask) == mask) return true;
            return false;
        }

        inline res_t three_consecutive_numbers(const hand& h) {
            uint16_t num_table = 0u;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti))
                    num_table |= 1 << tile(ti).num();
            for (uint16_t mask = 0b1110u; mask <= 0b1110000000u; mask <<= 1U)
                if ((num_table | mask) == mask) return true;
            return false;
        }

        inline res_t four_consecutive_numbers(const hand& h) {
            uint16_t num_table = 0u;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti))
                    num_table |= 1 << tile(ti).num();
            for (uint16_t mask = 0b11110u; mask <= 0b1111000000u; mask <<= 1U)
                if ((num_table | mask) == mask) return true;
            return false;
        }

        inline res_t gapped_numbers(const hand& h) {
            uint16_t num_table = 0u;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti))
                    num_table |= 1 << tile(ti).num();
            if (utils::popcount(num_table) <= 2u) return false;
            for (uint16_t mask = 0b1010101010u; mask <= 0b10101010100u; mask <<= 1U)
                if ((num_table | mask) == mask) return true;
            for (uint16_t mask = 0b10010010u; mask <= 0b1001001000u; mask <<= 1U)
                if ((num_table | mask) == mask) return true;
            return false;
        }

        inline res_v nine_numbers(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint32_t num_table = 0u;
                for (const meld& m : d.melds())
                    if (m.type() == meld_type::sequence)
                        num_table += 0b001001001u << ((m.tile().num() - 1) * 3);
                    else num_table += 0b001u << (m.tile().num() * 3);
                num_table += (0b001u << (d.pair().num() * 3));
                return num_table == 153391688u;
            });
        }

        inline res_v common_number(const hand& h) {
            for (tile_t ti : tile_set::honour_tiles)
            if (h.counter().count(ti)) return {false};
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                num_t num = d.pair().num();
                for (const meld& m : d.melds())
                    if (!m.contains(tile_set::tiles_of_number(num))) return false;
                return true;
            });
        }

        inline res_v quadruple_sequence(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (const meld& m : d.melds())
                    if (!utils::is_equivalent(m, d.melds()[0])) return false;
                return true;
            });
        }

        inline res_v triple_sequence(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t count = 0u;
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_equivalent(d.melds()[i], d.melds()[j])) ++count;
                return (count >= 3u);
            });
        }

        inline res_v two_double_sequences(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t count = 0u;
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_equivalent(d.melds()[i], d.melds()[j])) ++count;
                return (count == 2u || count > 3u);
            });
        }

        inline res_v double_sequence(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t count = 0u;
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_equivalent(d.melds()[i], d.melds()[j])) ++count;
                return (count >= 1u);
            });
        }
        
        inline res_v mixed_triple_triplet(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t count = 0u;
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_mixed_double_triplet(d.melds()[i], d.melds()[j])) ++count;
                return count == 3u;
            });
        }

        inline res_v mixed_double_triplet(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t count = 0u;
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_mixed_double_triplet(d.melds()[i], d.melds()[j])) ++count;
                return count >= 1u;
            });
        }

        inline res_v two_mixed_double_triplets(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t count = 0u;
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_mixed_double_triplet(d.melds()[i], d.melds()[j])) ++count;
                return count >= 2u;
            });
        }

        inline res_v mixed_triple_sequence(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (tile_t ti : {2_m, 3_m, 4_m, 5_m, 6_m, 7_m, 8_m})
                    if (utils::contains(d, {meld(ti, meld_type::sequence), meld(ti + 0b00100000u, meld_type::sequence), meld(ti + 0b10000000u, meld_type::sequence)})) 
                        return true;
                return false;
            });
        }

        inline res_v two_mixed_double_sequences(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t count = 0u;
                uint8_t visited = 0u;
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_mixed_double_sequence(d.melds()[i], d.melds()[j])) {
                            count += !(visited & ((1 << i) + (1 << j)));
                            if (!(visited & ((1 << i) + (1 << j)))) visited |= (1 << i) | (1 << j);
                        }
                return count >= 2u;
            });
        }

        inline res_v mixed_double_sequence(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t count = 0u;
                uint8_t visited = 0u;
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_mixed_double_sequence(d.melds()[i], d.melds()[j])) {
                            count += !(visited & ((1 << i) + (1 << j)));
                            if (!(visited & ((1 << i) + (1 << j)))) visited |= (1 << i) | (1 << j);
                        }
                return count >= 1u;
            });
        }

        inline res_v four_shifted_triplets(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (tile_t ti : {1_m, 2_m, 3_m, 4_m, 5_m, 6_m, 1_p, 2_p, 3_p, 4_p, 5_p, 6_p, 1_s, 2_s, 3_s, 4_s, 5_s, 6_s})
                    if (utils::contains(d, {meld(ti, meld_type::triplet), meld(ti + 1, meld_type::triplet), meld(ti + 2, meld_type::triplet), meld(ti + 3, meld_type::triplet)})) 
                        return true;
                return false;
            });
        }

        inline res_v three_shifted_triplets(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (tile_t ti : {1_m, 2_m, 3_m, 4_m, 5_m, 6_m, 7_m, 1_p, 2_p, 3_p, 4_p, 5_p, 6_p, 7_p, 1_s, 2_s, 3_s, 4_s, 5_s, 6_s, 7_s})
                    if (utils::contains(d, {meld(ti, meld_type::triplet), meld(ti + 1, meld_type::triplet), meld(ti + 2, meld_type::triplet)})) 
                        return true;
                return false;
            });
        }

        inline res_v four_shifted_sequences(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (tile_t ti : {2_m, 3_m, 4_m, 5_m, 2_p, 3_p, 4_p, 5_p, 2_s, 3_s, 4_s, 5_s})
                    if (utils::contains(d, {meld(ti, meld_type::sequence), meld(ti + 1, meld_type::sequence), meld(ti + 2, meld_type::sequence), meld(ti + 3, meld_type::sequence)})) 
                    return true;
                return false;
            });
        }

        inline res_v three_shifted_sequences(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (tile_t ti : {2_m, 3_m, 4_m, 5_m, 6_m, 2_p, 3_p, 4_p, 5_p, 6_p, 2_s, 3_s, 4_s, 5_s, 6_s, 7_s})
                    if (utils::contains(d, {meld(ti, meld_type::sequence), meld(ti + 1, meld_type::sequence), meld(ti + 2, meld_type::sequence)})) 
                        return true;
                return false;
            });
        }

        inline res_v four_chained_sequences(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (tile_t ti : {2_m, 2_p, 2_s})
                    if (utils::contains(d, {meld(ti, meld_type::sequence), meld(ti + 2, meld_type::sequence), meld(ti + 4, meld_type::sequence), meld(ti + 6, meld_type::sequence)})) 
                        return true;
                return false;
            });
        }

        inline res_v three_chained_sequences(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (tile_t ti : {2_m, 2_p, 2_s, 3_m, 3_p, 3_s, 4_m, 4_p, 4_s})
                    if (utils::contains(d, {meld(ti, meld_type::sequence), meld(ti + 2, meld_type::sequence), meld(ti + 4, meld_type::sequence)})) 
                        return true;
                return false;
            });
        }

        inline res_v pure_straight(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (tile_t ti : {2_m, 2_p, 2_s})
                    if (utils::contains(d, {meld(ti, meld_type::sequence), meld(ti + 3, meld_type::sequence), meld(ti + 6, meld_type::sequence)})) 
                        return true;
                return false;
            });
        }

        inline res_v two_short_straights(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t count = 0u;
                uint8_t visited = 0u;
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_shifted_sequences(d.melds()[i], d.melds()[j], 3)) {
                            count += !(visited & ((1 << i) + (1 << j)));
                            if (!(visited & ((1 << i) + (1 << j)))) visited |= (1 << i) | (1 << j);
                        }
                if (count == 2u) return true;
                return false;
            });
        }

        inline res_v short_straight(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t count = 0u;
                uint8_t visited = 0u;
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_shifted_sequences(d.melds()[i], d.melds()[j], 3)) {
                            count += !(visited & ((1 << i) + (1 << j)));
                            visited |= (1 << i) | (1 << j);
                        }
                return count >= 1u;
            });
        }

        inline res_v two_terminal_sequences(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_shifted_sequences(d.melds()[i], d.melds()[j], 6))
                            return true;
                return false;
            });
        }

        inline res_v mixed_shifted_triplets(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (auto& seq : patterns::mixed_shifted_triplets)
                    if (utils::contains(d, seq)) return true;
                return false;
            });
        }

        inline res_v mixed_shifted_sequences(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (auto& seq : patterns::mixed_shifted_sequences)
                    if (utils::contains(d, seq)) return true;
                return false;
            });
        }

        inline res_v mixed_chained_sequences(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (auto& seq : patterns::mixed_chained_sequences)
                    if (utils::contains(d, seq)) return true;
                return false;
            });
        }

        inline res_v mixed_straight(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (auto& seq : patterns::mixed_straight)
                    if (utils::contains(d, seq)) return true;
                return false;
            });
        }

        inline res_t seven_pairs(const hand& h) {
            return is_seven_pairs(h);
        }

        inline res_t big_seven_honours(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            return utils::contains_pair_of(h, tile_set::honour_tiles);
        }

        inline res_t four_wind_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            return utils::contains_pair_of(h, tile_set::wind_tiles);
        }

        inline res_t three_dragon_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            return utils::contains_pair_of(h, tile_set::dragon_tiles);
        }

        inline res_t six_dragon_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            return utils::count_pair_of(h, tile_set::dragon_tiles) == 6u;
        }

        inline res_t seven_shifted_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            for (tile_t ti : {1_m, 2_m, 3_m, 1_p, 2_p, 3_p, 1_s, 2_s, 3_s})
                if (utils::contains_pair_of(h, {
                    static_cast<tile_t>(ti), static_cast<tile_t>(ti + 1), static_cast<tile_t>(ti + 2), static_cast<tile_t>(ti + 3), 
                    static_cast<tile_t>(ti + 4), static_cast<tile_t>(ti + 5), static_cast<tile_t>(ti + 6)
                })) return true;
            return false;
        }

        inline res_t six_shifted_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            for (tile_t ti : {1_m, 2_m, 3_m, 4_m, 1_p, 2_p, 3_p, 4_p, 1_s, 2_s, 3_s, 4_s})
                if (utils::contains_pair_of(h, {
                    static_cast<tile_t>(ti), static_cast<tile_t>(ti + 1), static_cast<tile_t>(ti + 2), 
                    static_cast<tile_t>(ti + 3), static_cast<tile_t>(ti + 4), static_cast<tile_t>(ti + 5)
                })) return true;
            return false;
        }

        inline res_t five_shifted_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            for (tile_t ti : {1_m, 2_m, 3_m, 4_m, 5_m, 1_p, 2_p, 3_p, 4_p, 5_p, 1_s, 2_s, 3_s, 4_s, 5_s})
                if (utils::contains_pair_of(h, {
                    static_cast<tile_t>(ti), static_cast<tile_t>(ti + 1), static_cast<tile_t>(ti + 2), 
                    static_cast<tile_t>(ti + 3), static_cast<tile_t>(ti + 4)
                })) return true;
            return false;
        }

        inline res_t four_shifted_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            for (tile_t ti : {1_m, 2_m, 3_m, 4_m, 5_m, 6_m, 1_p, 2_p, 3_p, 4_p, 5_p, 6_p, 1_s, 2_s, 3_s, 4_s, 5_s, 6_s})
                if (utils::contains_pair_of(h, {
                    static_cast<tile_t>(ti), static_cast<tile_t>(ti + 1), static_cast<tile_t>(ti + 2), 
                    static_cast<tile_t>(ti + 3)
                })) return true;
            return false;
        }

        inline res_t reflected_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            const auto& c = h.counter();
            for (tile_t ti : tile_set::honour_tiles)
                if (c.count(ti)) return false;
            uint8_t min_num = 10u, max_num = 0u;
            for (tile_t ti : tile_set::numbered_tiles)
                if (c.count(ti)) {
                    min_num = std::min(min_num, tile(ti).num());
                    max_num = std::max(max_num, tile(ti).num());
                }
            const uint8_t ref = min_num + max_num;
            for (tile_t t : tile_set::numbered_tiles)
                if (c.count(t) && c.count(t) != c.count(utils::reflect_by(t, ref))) 
                    return false;
            return true;
        }

        inline res_t reflected_pairs_2(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            const auto& c = h.counter();
            for (tile_t ti : tile_set::honour_tiles)
                if (c.count(ti)) return false;
            uint8_t min_num = 10u, max_num = 0u;
            for (tile_t ti : tile_set::numbered_tiles)
                if (c.count(ti)) {
                    min_num = std::min(min_num, tile(ti).num());
                    max_num = std::max(max_num, tile(ti).num());
                }
            const uint8_t ref = min_num + max_num;
            uint8_t m_count = h.counter().count(tile_set::character_tiles);
            uint8_t p_count = h.counter().count(tile_set::dot_tiles);
            uint8_t s_count = h.counter().count(tile_set::bamboo_tiles);
            if (m_count != p_count && m_count != s_count && p_count != s_count) return false;
            suit_type s_ = mahjong::suit_type::z;
            if (m_count == p_count) s_ = suit_type::s;
            if (m_count == s_count) s_ = suit_type::p;
            if (p_count == s_count) s_ = suit_type::m;
            for (tile_t t : tile_set::numbered_tiles)
                if (c.count(t) && c.count(t) != c.count(utils::reflect_suit(utils::reflect_by(t, ref), s_))) 
                    return false;
            return true;
        }

        inline res_t two_triple_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            auto m = tile_literals::operator""_m;
            auto p = tile_literals::operator""_p;
            auto s = tile_literals::operator""_s;
            for (uint8_t i = 1; i <= 8; ++i)
                for (uint8_t j = i + 1; j <= 9; ++j)
                    if (utils::contains_pair_of(h, {m(i), p(i), s(i), m(j), p(j), s(j)})) return true;
            for (uint8_t i = 1; i <= 9; ++i)
                if (utils::count_pair_of(h, {m(i), p(i), s(i)}) == 6u) return true;
            return false;
        }

        inline res_t mixed_triple_pair(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            auto m = tile_literals::operator""_m;
            auto p = tile_literals::operator""_p;
            auto s = tile_literals::operator""_s;
            for (uint8_t i = 1; i <= 9; ++i) {
                if (utils::contains_pair_of(h, {m(i), p(i), s(i)})) return true;
            }
            return false;
        }

        inline res_t three_mirrored_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            auto m = tile_literals::operator""_m;
            auto p = tile_literals::operator""_p;
            auto s = tile_literals::operator""_s;
            for (uint8_t i = 1; i <= 7; ++i)
                for (uint8_t j = i + 1; j <= 8; ++j)
                    for (uint8_t k = j + 1; k <= 9; ++k) {
                        if (utils::contains_pair_of(h, {m(i), m(j), m(k), p(i), p(j), p(k)})) return true;
                        if (utils::contains_pair_of(h, {s(i), s(j), s(k), p(i), p(j), p(k)})) return true;
                        if (utils::contains_pair_of(h, {m(i), m(j), m(k), s(i), s(j), s(k)})) return true;
                    }
            for (uint8_t i = 1; i <= 8; ++i)
                for (uint8_t j = i + 1; j <= 9; ++j) {
                    if (utils::contains_pair_of(h, {m(i), p(i)}) && utils::count_pair_of(h, {m(j), p(j)}) == 4u) return true;
                    if (utils::contains_pair_of(h, {s(i), p(i)}) && utils::count_pair_of(h, {s(j), p(j)}) == 4u) return true;
                    if (utils::contains_pair_of(h, {m(i), s(i)}) && utils::count_pair_of(h, {m(j), s(j)}) == 4u) return true;
                    if (utils::contains_pair_of(h, {m(j), p(j)}) && utils::count_pair_of(h, {m(i), p(i)}) == 4u) return true;
                    if (utils::contains_pair_of(h, {s(j), p(j)}) && utils::count_pair_of(h, {s(i), p(i)}) == 4u) return true;
                    if (utils::contains_pair_of(h, {m(j), s(j)}) && utils::count_pair_of(h, {m(i), s(i)}) == 4u) return true;
                }
            return false;
        }

        inline res_t two_mirrored_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            auto m = tile_literals::operator""_m;
            auto p = tile_literals::operator""_p;
            auto s = tile_literals::operator""_s;
            for (uint8_t i = 1; i <= 8; ++i)
                for (uint8_t j = i + 1; j <= 9; ++j) {
                    if (utils::contains_pair_of(h, {m(i), m(j), p(i), p(j)})) return true;
                    if (utils::contains_pair_of(h, {s(i), s(j), p(i), p(j)})) return true;
                    if (utils::contains_pair_of(h, {m(i), m(j), s(i), s(j)})) return true;
                }
            for (uint8_t i = 1; i <= 9; ++i) {
                if (utils::count_pair_of(h, {m(i), p(i)}) == 4u) return true;
                if (utils::count_pair_of(h, {s(i), p(i)}) == 4u) return true;
                if (utils::count_pair_of(h, {m(i), s(i)}) == 4u) return true;
            }
            return false;
        }

        inline res_t eight_hog(const hand& h) {
            uint8_t count = 0u;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti) == 4) ++count;
            for (const auto& m : h.melds())
                if (m.type() == meld_type::kong) --count;
            return count >= 2u;
        }

        inline res_t twelve_hog(const hand& h) {
            uint8_t count = 0u;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti) == 4) ++count;
            for (const auto& m : h.melds())
                if (m.type() == meld_type::kong) --count;
            return count == 3u;
        }

        inline res_t disabled(const hand&) {
            return false;
        }

        inline res_v reflected_hand(const hand& h) {
            for (tile_t ti : tile_set::honour_tiles)
                if (h.counter().count(ti)) return {false};
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                return utils::is_reflection(d, d, 2 * d.pair().num());
            }, reflected_pairs);
        }

        inline res_v reflected_hand_2(const hand& h) {
            for (tile_t ti : tile_set::honour_tiles)
                if (h.counter().count(ti)) return {false};
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                return utils::is_reflection_2(d, d, 2 * d.pair().num());
            }, reflected_pairs_2);
        }

        inline res_v mirrored_hand(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d, const hand& /*unused_hand*/) {
                uint8_t suit_distribution = 0u;
                for (const meld& m : d.melds())
                    suit_distribution |= (1 << (m.tile().suit() >> 5));
                if (utils::popcount(suit_distribution) != 2u) return false;
                uint8_t count = 0u;
                uint8_t visited = 0u;
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_mixed_double_sequence(d.melds()[i], d.melds()[j]) || utils::is_mixed_double_triplet(d.melds()[i], d.melds()[j])) {
                            count += !(visited & ((1 << i) + (1 << j)));
                            if (!(visited & ((1 << i) + (1 << j)))) visited |= (1 << i) | (1 << j);
                        }
                return count == 2u;
            });
        }

        inline res_t three_shifted_hogs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            for (tile_t ti : tile_set::numbered_tiles) {
                if (tile(ti).num() > 7) continue;
                if (utils::count_pair_of(h, {tile(ti), tile(ti + 1), tile(ti + 2)}) == 6u) {
                    return true;
                }
            }
            return false;
        }

        inline res_t mixed_triple_hog(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            auto m = tile_literals::operator""_m;
            auto p = tile_literals::operator""_p;
            auto s = tile_literals::operator""_s;
            for (uint8_t i = 1; i <= 9; ++i) {
                if (utils::count_pair_of(h, {m(i), p(i), s(i)}) == 6u) {
                    return true;
                }
            }
            return false;
        }

        inline res_t mixed_shifted_hogs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            auto m = tile_literals::operator""_m;
            auto p = tile_literals::operator""_p;
            auto s = tile_literals::operator""_s;
            for (uint8_t i = 1; i <= 7; ++i) {
                if (utils::count_pair_of(h, {m(i), p(i + 1), s(i + 2)}) == 6u) {
                    return true;
                }
                if (utils::count_pair_of(h, {m(i), p(i + 2), s(i + 1)}) == 6u) {
                    return true;
                }
                if (utils::count_pair_of(h, {m(i + 1), p(i), s(i + 2)}) == 6u) {
                    return true;
                }
                if (utils::count_pair_of(h, {m(i + 2), p(i), s(i + 1)}) == 6u) {
                    return true;
                }
                if (utils::count_pair_of(h, {m(i + 1), p(i + 2), s(i)}) == 6u) {
                    return true;
                }
                if (utils::count_pair_of(h, {m(i + 2), p(i + 1), s(i)}) == 6u) {
                    return true;
                }
            }
            return false;
        }

        inline res_t seven_wind_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            return utils::count_pair_of(h, tile_set::wind_tiles) == 7u;
        }

        inline res_v connected_numbers(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (tile_t ti : tile_set::honour_tiles)
                    if (d.counter().count(ti)) return false;
                uint32_t num_table = (1 << (3 * d.pair().num()));
                for (const meld& m : d.melds()) {
                    if (m.type() == meld_type::sequence) {
                        num_table += (1 << (3 * m.tile().num()));
                        num_table += (1 << (3 * (m.tile().num() + 1)));
                        num_table += (1 << (3 * (m.tile().num() - 1)));
                    } else {
                        num_table += (1 << (3 * m.tile().num()));
                    }
                }
                char t = 0;
                while (num_table) {
                    auto c = num_table & 0b111u;
                    if (c > 1) return false;
                    if (t == 2 && c == 1) return false;
                    if (t == 1 && c == 0) t = 2;
                    if (t == 0 && c == 1) t = 1;
                    num_table >>= 3;
                }
                return true;
            }, [](const hand& h) {
                for (tile_t ti : tile_set::honour_tiles)
                    if (h.counter().count(ti)) return false;
                if (!is_seven_pairs(h)) return false;
                uint32_t num_table = 0u;
                for (tile_t ti : tile_set::numbered_tiles)
                    num_table += (h.counter().count(ti) / 2) << (3 * tile(ti).num());
                char t = 0;
                while (num_table) {
                    auto c = num_table & 0b111u;
                    if (c > 1) return false;
                    if (t == 2 && c == 1) return false;
                    if (t == 1 && c == 0) t = 2;
                    if (t == 0 && c == 1) t = 1;
                    num_table >>= 3;
                }
                return true;
            });
        }

        inline res_t mixed_one_number(const hand& h) {
            uint16_t num_table = 0u;
            for (tile_t ti : tile_set::numbered_tiles)
                if (h.counter().count(ti))
                    num_table |= (1u << tile(ti).num());
            return utils::popcount(num_table) <= 1u;
        }

        inline res_t trivial(const hand& /*unused_hand*/) {
            return true;
        }

        inline res_t double_pair(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti) == 4) return true;
            return false;
        }

        inline res_t two_double_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            uint8_t count = 0u;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti) == 4) ++count;
            return count >= 2u;
        }

        inline res_t three_double_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            uint8_t count = 0u;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti) == 4) ++count;
            return count >= 3u;
        }

        inline res_v three_wind_triplets(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t count = 0u;
                for (tile_t ti : tile_set::wind_tiles)
                    if (utils::contains(d, {meld(ti, meld_type::triplet)})) 
                        ++count;
                return count >= 3;
            });
        }

        inline res_t six_wind_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            return utils::count_pair_of(h, tile_set::wind_tiles) >= 6;
        }

        inline res_t five_wind_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            return utils::count_pair_of(h, tile_set::wind_tiles) >= 5;
        }

        inline res_t four_wind_pairs_2(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            return utils::count_pair_of(h, tile_set::wind_tiles) >= 4;
        }

        inline res_t thirteen_orphans(const hand& h) {
            const std::vector<tile_t> required_tiles = {
                1_m, 9_m, 1_p, 9_p, 1_s, 9_s, 1_z, 2_z, 3_z, 4_z, 5_z, 6_z, 7_z
            };
            for (tile_t ti : required_tiles) {
                if (h.counter().count(ti) == 0) return false;
            }
            return true;
        }

    }

    const std::vector<fan> fans = {
        fan("和牌", {1, 0, 0, 0}, criteria::trivial),
        fan("天和", {1, 0, 1, 16}, criteria::heavenly_hand),
        fan("地和", {1, 0, 1, 16}, criteria::earthly_hand),
        fan("岭上开花", {0, 0, 1, 4}, criteria::out_with_replacement_tile),
        fan("海底捞月", {1, 0, 1, 4}, criteria::last_tile_draw),
        fan("河底捞鱼", {1, 0, 1, 4}, criteria::last_tile_claim),
        fan("抢杠", {1, 0, 1, 4}, criteria::robbing_the_kong),
        fan("十三幺", {1, 1, 0, 0}, criteria::thirteen_orphans),
        fan("七对", {1, 1, 0, 0}, criteria::seven_pairs),
        fan("门前清", {1, 0, 0, 0}, criteria::concealed_hand),
        fan("四暗杠", {}, criteria::four_concealed_kongs),
        fan("三暗杠", {}, criteria::three_concealed_kongs),
        fan("双暗杠", {}, criteria::two_concealed_kongs),
        fan("暗杠", {}, criteria::concealed_kong),
        fan("四杠", {}, criteria::four_kongs),
        fan("三杠", {}, criteria::three_kongs),
        fan("双杠", {}, criteria::two_kongs),
        fan("四暗刻", {}, criteria::four_concealed_triplets),
        fan("三暗刻", {}, criteria::three_concealed_triplets),
        fan("对对和", {}, criteria::all_triplets),
        fan("十二归", {1, 0, 0, 0}, criteria::twelve_hog),
        fan("八归", {1, 0, 0, 0}, criteria::eight_hog),
        fan("三叠对", {1, 1, 0, 0}, criteria::three_double_pairs),
        fan("二叠对", {1, 1, 0, 0}, criteria::two_double_pairs),
        fan("叠对", {1, 1, 0, 0}, criteria::double_pair),
        fan("字一色", {1, 0, 0, 0}, criteria::all_honours),
        fan("大四喜", {}, criteria::big_four_winds),
        fan("小四喜", {}, criteria::little_four_winds),
        fan("四喜对", {1, 1, 0, 0}, criteria::four_wind_pairs),
        fan("风牌三刻", {}, criteria::three_wind_triplets),
        fan("风牌七对", {1, 1, 0, 0}, criteria::seven_wind_pairs),
        fan("风牌六对", {1, 1, 0, 0}, criteria::six_wind_pairs),
        // fan("风牌五对", {1, 1, 0, 0}, criteria::disabled),
        // fan("风牌四对", {1, 1, 0, 0}, criteria::disabled),
        fan("大三元", {}, criteria::big_three_dragons),
        fan("小三元", {}, criteria::little_three_dragons),
        fan("三元六对", {1, 1, 0, 0}, criteria::six_dragon_pairs),
        fan("三元对", {1, 1, 0, 0}, criteria::three_dragon_pairs),
        fan("番牌四刻", {}, criteria::fan_tile_4t),
        fan("番牌三刻", {}, criteria::fan_tile_3t),
        fan("番牌二刻", {}, criteria::fan_tile_2t),
        fan("番牌刻", {}, criteria::fan_tile_1t),
        fan("番牌七对", {1, 1, 0, 0}, criteria::fan_tile_7p),
        fan("番牌六对", {1, 1, 0, 0}, criteria::fan_tile_6p),
        fan("番牌五对", {1, 1, 0, 0}, criteria::fan_tile_5p),
        fan("番牌四副", {1, 0, 0, 0}, criteria::fan_tile_4p),
        fan("番牌三副", {1, 0, 0, 0}, criteria::fan_tile_3p),
        fan("番牌二副", {1, 0, 0, 0}, criteria::fan_tile_2p),
        fan("番牌", {1, 0, 0, 0}, criteria::fan_tile_1p),
        fan("清幺九", {1, 0, 0, 0}, criteria::all_terminals),
        fan("混幺九", {1, 0, 0, 0}, criteria::all_terminals_and_honours),
        fan("清带幺", {1, 0, 0, 0}, criteria::pure_outside_hand),
        fan("混带幺", {1, 0, 0, 0}, criteria::mixed_outside_hand),
        fan("九莲宝灯", {}, criteria::nine_gates),
        fan("清一色", {1, 0, 0, 0}, criteria::full_flush),
        fan("混一色", {1, 0, 0, 0}, criteria::half_flush),
        fan("五门齐", {}, criteria::all_types),
        fan("混一数", {1, 0, 0, 0}, criteria::mixed_one_number),
        fan("二数", {1, 0, 0, 0}, criteria::two_numbers),
        fan("二聚", {1, 0, 0, 0}, criteria::two_consecutive_numbers),
        fan("三聚", {1, 0, 0, 0}, criteria::three_consecutive_numbers),
        fan("四聚", {1, 0, 0, 0}, criteria::four_consecutive_numbers),
        fan("连数", {1, 0, 0, 0}, criteria::connected_numbers),
        fan("间数", {1, 0, 0, 0}, criteria::gapped_numbers),
        fan("镜数", {1, 0, 0, 0}, criteria::reflected_hand),
        fan("映数", {1, 0, 0, 0}, criteria::reflected_hand_2),
        fan("满庭芳", {}, criteria::common_number),
        fan("四同顺", {}, criteria::quadruple_sequence),
        fan("三同顺", {}, criteria::triple_sequence),
        fan("二般高", {}, criteria::two_double_sequences),
        fan("一般高", {}, criteria::double_sequence),
        fan("四连刻", {}, criteria::four_shifted_triplets),
        fan("三连刻", {}, criteria::three_shifted_triplets),
        fan("四步高", {}, criteria::four_shifted_sequences),
        fan("三步高", {}, criteria::three_shifted_sequences),
        fan("四连环", {}, criteria::four_chained_sequences),
        fan("三连环", {}, criteria::three_chained_sequences),
        fan("一气贯通", {}, criteria::pure_straight),
        fan("七连对", {1, 1, 0, 0}, criteria::seven_shifted_pairs),
        fan("六连对", {1, 1, 0, 0}, criteria::six_shifted_pairs),
        fan("五连对", {1, 1, 0, 0}, criteria::five_shifted_pairs),
        fan("四连对", {1, 1, 0, 0}, criteria::four_shifted_pairs),
        fan("三色同刻", {}, criteria::mixed_triple_triplet),
        fan("三色同顺", {}, criteria::mixed_triple_sequence),
        fan("三色二对", {1, 1, 0, 0}, criteria::two_triple_pairs),
        fan("三色同对", {1, 1, 0, 0}, criteria::mixed_triple_pair),
        fan("三色连刻", {}, criteria::mixed_shifted_triplets),
        fan("三色贯通", {}, criteria::mixed_straight),
        fan("镜同", {}, criteria::mirrored_hand),
        fan("镜同三对", {1, 1, 0, 0}, criteria::three_mirrored_pairs),
        fan("镜同二对", {1, 1, 0, 0}, criteria::two_mirrored_pairs),
        fan("双龙会", {}, criteria::two_short_straights)
    };

    std::vector<fan_code> evaluate_fans(const hand& h, bool ignore_occ) {
        std::vector<criteria::res_v> fan_results;
        std::vector<fan_code> results;
        for (const auto& fan : fans)
            fan_results.push_back(fan(h));
        for (std::size_t i = 0; i < h.decompose().size(); ++i) {
            fan_code res;
            for (std::size_t j = 0; j < fan_results.size(); ++j) {
                if (fans[j].tag.is_special || (ignore_occ && fans[j].tag.is_occasional)) continue;
                if (fan_results[j].size() == 1) res[j] = fan_results[j][0];
                else res[j] = fan_results[j][i + 1];   
            }
            results.push_back(res);
        }
        if (is_seven_pairs(h) || is_thirteen_orphans(h)) {
            fan_code res;
            for (std::size_t j = 0; j < fan_results.size(); ++j)
                if (fans[j].tag.special_compatible && (!ignore_occ || !fans[j].tag.is_occasional)) 
                    res[j] = fan_results[j][0];
            results.push_back(res);
        }
        return results;
    }

    bool has_fan(const hand& h) {
        bool first = true;
        for (const auto& fan : fans) {
            if (first) {
                first = false;
                continue;
            }
            auto r = fan(h);
            for (uint8_t _ : r)
                if (_) return true;
        }
        return false;
    }

    unsigned long long get_weight(const w_data& data, const fan_code& res) {
        return data.fan_cache.contains(res) ? 1ULL : 0ULL;
    }

    double get_fan(const w_data& data, const fan_code& res) {
        double fixed_fan = 0.0;
        auto res1 = res;
        for (int i = indices::heavenly_hand; i <= indices::robbing_the_kong; ++i) {
            fixed_fan += res1[i] ? fans[i].tag.fan_value : 0.0;
            res1[i] = false;
        }

        if (!res1.any()) {
            return fixed_fan;
        }

        if (const auto exact = data.fan_cache.find(res); exact != data.fan_cache.end()) {
            return exact->second;
        }
        if (const auto stripped = data.fan_cache.find(res1); stripped != data.fan_cache.end()) {
            return fixed_fan + stripped->second;
        }

        std::cerr << "Missing fan_cache entry for code " << res1 << ". Returning 0.\n";
        return 0.0;
    }

    std::pair<double, fan_code> get_fan(const w_data& data, const hand& h) {
        auto v0 = evaluate_fans(h, false);
        auto v = v0;
        for (int i = indices::heavenly_hand; i <= indices::robbing_the_kong; ++i)
            for (std::size_t j = 0; j < v0.size(); ++j)
                v[j][i] = false;
        double max_fan = 0.0;
        fan_code max_res;
        auto is_greater_than = [](double a, double b) {
            if (a - b > 1e-3) return true;
            if (std::round(a * a) > std::round(b * b) + 1e-3) return true;
            if (std::round(10 * a * std::tanh(0.1 * a)) > std::round(10 * b * std::tanh(0.1 * b)) + 1e-3) return true;
            return false;
        };
        for (const auto& res : v) {
            double fan = get_fan(data, res);
            if (is_greater_than(fan, max_fan)) { 
                max_fan = fan;
                max_res = res;
            }
        }
        for (int i = 0; i <= indices::robbing_the_kong; ++i) {
            max_res[i] = v0[0][i];
            max_fan += v0[0][i] ? fans[i].tag.fan_value : 0.0;
        }
        return {max_fan, max_res};
    }

    struct cover_ {
        const fan_code& res;
        fan_code& new_res;
        cover_(const fan_code& res, fan_code& new_res) : res(res), new_res(new_res) {}

        template<typename... Indices>
        void operator()(qingque::indices i, Indices... j) {
            static_cast<void>(i);
            ((new_res[j] = new_res[j] && !res[i]), ...);
        }
    };

    // Simplify fan structure while preserving the meaning of the fan result. 
    // For example, {fan_tile_2p, fan_tile_1p} -> {fan_tile_2p}
    fan_code derepellenise(const fan_code& res) {
        fan_code new_res = res;
        using enum indices;
        cover_ cover(res, new_res);

        new_res[trivial] = false;

        cover(heavenly_hand, concealed_hand);
        cover(earthly_hand, concealed_hand);
        
        cover(seven_pairs, concealed_hand);

        cover(four_concealed_kongs, 
            three_concealed_kongs, two_concealed_kongs, concealed_kong, 
            four_kongs, three_kongs, two_kongs, 
            four_concealed_triplets, three_concealed_triplets, all_triplets, 
            concealed_hand);
        cover(three_concealed_kongs,
            two_concealed_kongs, concealed_kong,
            three_kongs, two_kongs, three_concealed_triplets);
        cover(two_concealed_kongs, concealed_kong, two_kongs);
        cover(four_kongs, three_kongs, two_kongs, all_triplets);
        cover(three_kongs, two_kongs);
        cover(four_concealed_triplets, three_concealed_triplets, all_triplets, concealed_hand);

        cover(twelve_hog, eight_hog);
        
        cover(all_honours, half_flush, mixed_one_number, all_terminals_and_honours, mixed_outside_hand, fan_tile_1p);
        cover(big_four_winds, three_wind_triplets, all_triplets, mixed_one_number, half_flush, fan_tile_1t);
        cover(little_four_winds, three_wind_triplets, half_flush, fan_tile_1p);
        cover(four_wind_pairs, fan_tile_1p);
        cover(seven_wind_pairs, six_wind_pairs, four_wind_pairs, fan_tile_1p, all_honours, twelve_hog, three_double_pairs);
        cover(six_wind_pairs, half_flush, mixed_one_number, eight_hog, two_double_pairs);
        cover(big_three_dragons, fan_tile_3t);
        cover(little_three_dragons, fan_tile_2t, fan_tile_3p);
        cover(six_dragon_pairs, three_dragon_pairs, fan_tile_6p, 
            half_flush, mixed_one_number, twelve_hog, three_double_pairs);
        cover(three_dragon_pairs, fan_tile_3p);
        cover(fan_tile_4t, fan_tile_3t, fan_tile_2t, fan_tile_1t,
            fan_tile_4p, fan_tile_3p, fan_tile_2p, fan_tile_1p,
            half_flush, mixed_one_number, all_triplets);
        cover(fan_tile_3t, fan_tile_2t, fan_tile_1t,
            fan_tile_3p, fan_tile_2p, fan_tile_1p);
        cover(fan_tile_2t, fan_tile_1t, fan_tile_2p, fan_tile_1p);
        cover(fan_tile_1t, fan_tile_1p);
        cover(fan_tile_7p, fan_tile_6p, fan_tile_5p, fan_tile_4p, fan_tile_3p, fan_tile_2p, fan_tile_1p,
            all_honours, twelve_hog, three_double_pairs, three_dragon_pairs);
        cover(fan_tile_6p, fan_tile_5p, fan_tile_4p, fan_tile_3p, fan_tile_2p, fan_tile_1p,
            half_flush, mixed_one_number, eight_hog, two_double_pairs);
        cover(fan_tile_5p, fan_tile_4p, fan_tile_3p, fan_tile_2p, fan_tile_1p, double_pair);
        cover(fan_tile_4p, fan_tile_3p, fan_tile_2p, fan_tile_1p);
        cover(fan_tile_3p, fan_tile_2p, fan_tile_1p);
        cover(fan_tile_2p, fan_tile_1p);
        
        cover(all_terminals, all_terminals_and_honours, pure_outside_hand, mixed_outside_hand, two_numbers);
        cover(all_terminals_and_honours, mixed_outside_hand);
        cover(pure_outside_hand, mixed_outside_hand);
        
        cover(nine_gates, full_flush, concealed_hand);
        cover(full_flush, half_flush);
        cover(all_types, fan_tile_1p);
        
        cover(two_consecutive_numbers, two_numbers);
        
        if (res[reflected_hand] && res[reflected_hand_2] && res[full_flush]) {
            new_res[reflected_hand_2] = false;
        }
        
        cover(quadruple_sequence, triple_sequence, two_double_sequences, double_sequence, twelve_hog);
        cover(triple_sequence, double_sequence);
        cover(two_double_sequences, double_sequence);
        cover(four_shifted_triplets, three_shifted_triplets, all_triplets);
        cover(four_shifted_sequences, three_shifted_sequences);
        cover(four_chained_sequences, three_chained_sequences);
        // cover(three_shifted_hogs, twelve_hog);
        cover(seven_shifted_pairs, six_shifted_pairs, five_shifted_pairs, four_shifted_pairs, 
            full_flush, reflected_hand, connected_numbers);
        cover(six_shifted_pairs, five_shifted_pairs, four_shifted_pairs);
        cover(five_shifted_pairs, four_shifted_pairs);
        // cover(mixed_triple_hog, mixed_triple_pair, two_triple_pairs, twelve_hog);
        cover(two_triple_pairs, mixed_triple_pair, two_mirrored_pairs);
        // cover(mixed_shifted_hogs, twelve_hog);
        cover(three_mirrored_pairs, two_mirrored_pairs);

        cover(two_consecutive_numbers, three_consecutive_numbers, four_consecutive_numbers);
        cover(three_consecutive_numbers, four_consecutive_numbers);
        cover(three_double_pairs, two_double_pairs, double_pair, twelve_hog, eight_hog);
        cover(two_double_pairs, double_pair, eight_hog);

        cover(thirteen_orphans, all_terminals_and_honours, concealed_hand);

        return new_res;
    }

    // Reduced simplification for statistics purposes
    fan_code derepellenise2(const fan_code& res) {
        fan_code new_res = res;
        using enum indices;
        cover_ cover(res, new_res);

        cover(four_concealed_kongs, 
            three_concealed_kongs, two_concealed_kongs, concealed_kong);
        cover(three_concealed_kongs, two_concealed_kongs, concealed_kong);
        cover(two_concealed_kongs, concealed_kong);
        cover(four_kongs, three_kongs, two_kongs);
        cover(three_kongs, two_kongs);
        cover(four_concealed_triplets, three_concealed_triplets);

        cover(twelve_hog, eight_hog);
        
        cover(all_honours, half_flush, mixed_one_number, all_terminals_and_honours, mixed_outside_hand);
        cover(big_four_winds, three_wind_triplets);
        cover(little_four_winds, three_wind_triplets);
        cover(seven_wind_pairs, six_wind_pairs);
        cover(six_wind_pairs);
        cover(six_dragon_pairs, three_dragon_pairs);
        cover(fan_tile_4t, fan_tile_3t, fan_tile_2t, fan_tile_1t);
        cover(fan_tile_3t, fan_tile_2t, fan_tile_1t);
        cover(fan_tile_2t, fan_tile_1t);
        cover(fan_tile_7p, fan_tile_6p, fan_tile_5p, fan_tile_4p, fan_tile_3p, fan_tile_2p, fan_tile_1p);
        cover(fan_tile_6p, fan_tile_5p, fan_tile_4p, fan_tile_3p, fan_tile_2p, fan_tile_1p);
        cover(fan_tile_5p, fan_tile_4p, fan_tile_3p, fan_tile_2p, fan_tile_1p);
        cover(fan_tile_4p, fan_tile_3p, fan_tile_2p, fan_tile_1p);
        cover(fan_tile_3p, fan_tile_2p, fan_tile_1p);
        cover(fan_tile_2p, fan_tile_1p);
        
        cover(all_terminals, all_terminals_and_honours, pure_outside_hand, mixed_outside_hand);
        cover(all_terminals_and_honours, mixed_outside_hand);
        cover(pure_outside_hand, mixed_outside_hand);
        
        cover(full_flush, half_flush);
        
        cover(quadruple_sequence, triple_sequence, two_double_sequences, double_sequence);
        cover(triple_sequence, double_sequence);
        cover(two_double_sequences, double_sequence);
        cover(four_shifted_triplets, three_shifted_triplets);
        cover(four_shifted_sequences, three_shifted_sequences);
        cover(four_chained_sequences, three_chained_sequences);
        cover(seven_shifted_pairs, six_shifted_pairs, five_shifted_pairs, four_shifted_pairs);
        cover(six_shifted_pairs, five_shifted_pairs, four_shifted_pairs);
        cover(five_shifted_pairs, four_shifted_pairs);
        // cover(mixed_triple_hog, mixed_triple_pair, two_triple_pairs);
        cover(two_triple_pairs, mixed_triple_pair, two_mirrored_pairs);
        cover(three_mirrored_pairs, two_mirrored_pairs);

        cover(two_consecutive_numbers, three_consecutive_numbers, four_consecutive_numbers);
        cover(three_consecutive_numbers, four_consecutive_numbers);
        cover(three_double_pairs, two_double_pairs, double_pair);
        cover(two_double_pairs, double_pair);

        return new_res;
    }

    std::vector<fan_code> derepellenise(const std::vector<fan_code>& res) {
        std::vector<fan_code> new_res;
        for (const auto& r : res) {
            new_res.push_back(derepellenise(r));
        }
        return new_res;
    }

    std::vector<fan_code> derepellenise2(const std::vector<fan_code>& res) {
        std::vector<fan_code> new_res;
        for (const auto& r : res) {
            new_res.push_back(derepellenise2(r));
        }
        return new_res;
    }

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