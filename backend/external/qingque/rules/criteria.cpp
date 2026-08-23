#include "../basic/mahjong.h"
#include "../basic/mahjong_utils.h"
#include "qingque.h"
#include "criteria.h"

namespace qingque {

    using namespace mahjong;
    using namespace tile_literals;

    namespace criteria {

        res_t heavenly_hand(const hand& h) {
            return h.winning_type()(win_type::heavenly_or_earthly_hand | win_type::self_drawn);
        }

        res_t earthly_hand(const hand& h) {
            return h.winning_type()(win_type::heavenly_or_earthly_hand, win_type::self_drawn);
        }

        res_t out_with_replacement_tile(const hand& h) {
            return h.winning_type()(win_type::kong_related | win_type::self_drawn);
        }

        res_t last_tile_draw(const hand& h) {
            return h.winning_type()(win_type::final_tile | win_type::self_drawn);
        }

        res_t last_tile_claim(const hand& h) {
            return h.winning_type()(win_type::final_tile, win_type::self_drawn);
        }

        res_t robbing_the_kong(const hand& h) {
            return h.winning_type()(win_type::kong_related, win_type::self_drawn);
        }

        res_t self_drawn(const hand& h) {
            return h.winning_type()(win_type::self_drawn);
        }

        res_t concealed_hand(const hand& h) {
            for (const auto& m : h.melds())
                if (!m.concealed()) return false;
            return true;
        }

        res_v four_concealed_kongs(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (const meld& m : d.melds())
                    if (!m.concealed() || m.type() != meld_type::kong) return false;
                return true;
            });
        }

        res_v three_concealed_kongs(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t counter = 0;
                for (const meld& m : d.melds())
                    if (m.concealed() && m.type() == meld_type::kong) ++counter;
                return counter >= 3;
            });
        }

        res_v two_concealed_kongs(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t counter = 0;
                for (const meld& m : d.melds())
                    if (m.concealed() && m.type() == meld_type::kong) ++counter;
                return counter >= 2;
            });
        }

        res_v concealed_kong(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t counter = 0;
                for (const meld& m : d.melds())
                    if (m.concealed() && m.type() == meld_type::kong) ++counter;
                return counter >= 1;
            });
        }

        res_t four_kongs(const hand& h) {
            return h.counter().count() >= 18;
        }

        res_t three_kongs(const hand& h) {
            return h.counter().count() >= 17;
        }

        res_t two_kongs(const hand& h) {
            return h.counter().count() >= 16;
        }

        res_t kong(const hand& h) {
            return h.counter().count() >= 15;
        }

        res_v four_concealed_triplets(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (const meld& m : d.melds())
                    if (!m.concealed() || m.type() == meld_type::sequence) return false;
                return true;
            });
        }

        res_v three_concealed_triplets(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t counter = 0;
                for (const meld& m : d.melds())
                    if (m.concealed() && m.type() > meld_type::sequence) ++counter;
                return counter >= 3;
            });
        }

        res_v two_concealed_triplets(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t counter = 0;
                for (const meld& m : d.melds())
                    if (m.concealed() && m.type() > meld_type::sequence) ++counter;
                return counter >= 2;
            });
        }

        res_v concealed_triplet(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t counter = 0;
                for (const meld& m : d.melds())
                    if (m.concealed() && m.type() > meld_type::sequence) ++counter;
                return counter >= 1;
            });
        }

        res_v all_triplets(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (const meld& m : d.melds())
                    if (m.type() == meld_type::sequence) return false;
                return true;
            });
        }

        res_t all_honours(const hand& h) {
            for (tile_t ti : tile_set::numbered_tiles)
            if (h.counter().count(ti)) return false;
            return true;
        }

        res_t big_four_winds(const hand& h) {
            for (tile_t ti : tile_set::wind_tiles)
            if (h.counter().count(ti) < 3) return false;
            return true;
        }

        res_t little_four_winds(const hand& h) {
            uint8_t count = 0u;
            for (tile_t ti : tile_set::wind_tiles)
            count += (h.counter().count(ti) >= 3) + (h.counter().count(ti) >= 2);
            return count == 7;
        }

        res_t big_three_dragons(const hand& h) {
            for (tile_t ti : tile_set::dragon_tiles)
            if (h.counter().count(ti) < 3) return false;
            return true;
        }

        res_t little_three_dragons(const hand& h) {
            uint8_t count = 0u;
            for (tile_t ti : tile_set::dragon_tiles)
            count += (h.counter().count(ti) >= 3) + (h.counter().count(ti) >= 2);
            return count == 5;
        }

        res_t fan_tile_1p(const hand& h) {
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            for (tile_t ti : fan_tiles)
                if (h.counter().count(ti) >= 2) return true;
            return false;
        }

        res_v fan_tile_2p(const hand& h) {
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

        res_v fan_tile_3p(const hand& h) {
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

        res_v fan_tile_4p(const hand& h) {
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

        res_t fan_tile_5p(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            return utils::count_pair_of(h, fan_tiles) >= 5;
        }

        res_t fan_tile_6p(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            return utils::count_pair_of(h, fan_tiles) >= 6;
        }

        res_t fan_tile_7p(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            return utils::count_pair_of(h, fan_tiles) >= 7;
        }

        res_t fan_tile_1t(const hand& h) {
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            uint8_t cnt = 0;
            for (tile_t ti : fan_tiles)
                cnt += (h.counter().count(ti) >= 3);
            return cnt >= 1;
        }

        res_t fan_tile_2t(const hand& h) {
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            uint8_t cnt = 0;
            for (tile_t ti : fan_tiles)
                cnt += (h.counter().count(ti) >= 3);
            return cnt >= 2;
        }

        res_t fan_tile_3t(const hand& h) {
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            uint8_t cnt = 0;
            for (tile_t ti : fan_tiles)
                cnt += (h.counter().count(ti) >= 3);
            return cnt >= 3;
        }

        res_t fan_tile_4t(const hand& h) {
            const std::array<tile_t, 4> fan_tiles = {h.winning_type().seat_wind(), honours::C, honours::F, honours::P};
            uint8_t cnt = 0;
            for (tile_t ti : fan_tiles)
                cnt += (h.counter().count(ti) >= 3);
            return cnt >= 4;
        }

        res_t all_terminals(const hand& h) {
            for (tile_t ti : tile_set::all_tiles)
            if (h.counter().count(ti) && tile(ti).num() != 1 && tile(ti).num() != 9) return false;
            return true;
        }

        res_t all_terminals_and_honours(const hand& h) {
            for (tile_t ti : tile_set::simple_tiles)
                if (h.counter().count(ti)) return false;
            return true;
        }

        res_v pure_outside_hand(const hand& h) {
            auto poh_check = [](const hand::decomposition& d) {
                if (!d.pair().is_in(tile_set::terminal_tiles)) return false;
                for (const meld& m : d.melds())
                    if (!m.contains(tile_set::terminal_tiles)) return false;
                return true;
            };
            return utils::for_all_decompositions<res_t>(h, poh_check, all_terminals);
        }

        res_v mixed_outside_hand(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                if (!d.pair().is_in(tile_set::terminal_honour_tiles)) return false;
                for (const meld& m : d.melds())
                    if (!m.contains(tile_set::terminal_honour_tiles)) return false;
                return true;
            }, all_terminals_and_honours);
        }

        res_t nine_gates(const hand& h) {
            auto c = h.counter(false);
            c.add(h.winning_tile(), -1);
            return (c == std::make_pair(patterns::nine_gates_m_s, 0ull) || 
                    c == std::make_pair(0ull, patterns::nine_gates_m_s) || 
                    c == std::make_pair(patterns::nine_gates_p, 0ull));
        }

        res_t full_flush(const hand& h) {
            for (tile_t ti : tile_set::honour_tiles)
                if (h.counter().count(ti)) return false;
            auto check_suit = [](const hand& h, suit_type st) {
                for (tile_t ti : tile_set::tiles_of_suit(st))
                    if (h.counter().count(ti)) return true;
                return false;
            };
            return (check_suit(h, suit_type::m) + check_suit(h, suit_type::p) + check_suit(h, suit_type::s) == 1);
        }

        res_t half_flush(const hand& h) {
            auto check_suit = [](const hand& h, suit_type st) {
                for (tile_t ti : tile_set::tiles_of_suit(st))
                    if (h.counter().count(ti)) return true;
                return false;
            };
            return (check_suit(h, suit_type::m) + check_suit(h, suit_type::p) + check_suit(h, suit_type::s) <= 1);
        }

        res_t all_types(const hand& h) {
            if (is_seven_pairs(h) || is_thirteen_orphans(h)) return false;
            if (!h.counter().count(tile_set::character_tiles)) return false;
            if (!h.counter().count(tile_set::bamboo_tiles)) return false;
            if (!h.counter().count(tile_set::dot_tiles)) return false;
            if (!h.counter().count(tile_set::wind_tiles)) return false;
            if (!h.counter().count(tile_set::dragon_tiles)) return false;
            return true;
        }

        res_t two_numbers(const hand& h) {
            uint16_t num_table = 0u;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti)) {
                    if (tile(ti).num() == 0) return false;
                    num_table |= 1 << tile(ti).num();
                }
            return utils::popcount(num_table) == 2u;
        }

        res_t two_consecutive_numbers(const hand& h) {
            uint16_t num_table = 0u;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti))
                    num_table |= 1 << tile(ti).num();
            for (uint16_t mask = 0b110u; mask <= 0b1100000000u; mask <<= 1U)
                if ((num_table | mask) == mask) return true;
            return false;
        }

        res_t three_consecutive_numbers(const hand& h) {
            uint16_t num_table = 0u;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti))
                    num_table |= 1 << tile(ti).num();
            for (uint16_t mask = 0b1110u; mask <= 0b1110000000u; mask <<= 1U)
                if ((num_table | mask) == mask) return true;
            return false;
        }

        res_t four_consecutive_numbers(const hand& h) {
            uint16_t num_table = 0u;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti))
                    num_table |= 1 << tile(ti).num();
            for (uint16_t mask = 0b11110u; mask <= 0b1111000000u; mask <<= 1U)
                if ((num_table | mask) == mask) return true;
            return false;
        }

        res_t gapped_numbers(const hand& h) {
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

        res_v nine_numbers(const hand& h) {
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

        res_v common_number(const hand& h) {
            for (tile_t ti : tile_set::honour_tiles)
            if (h.counter().count(ti)) return {false};
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                num_t num = d.pair().num();
                for (const meld& m : d.melds())
                    if (!m.contains(tile_set::tiles_of_number(num))) return false;
                return true;
            });
        }

        res_v quadruple_sequence(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (const meld& m : d.melds())
                    if (!utils::is_equivalent(m, d.melds()[0])) return false;
                return true;
            });
        }

        res_v triple_sequence(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t count = 0u;
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_equivalent(d.melds()[i], d.melds()[j])) ++count;
                return (count >= 3u);
            });
        }

        res_v two_double_sequences(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t count = 0u;
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_equivalent(d.melds()[i], d.melds()[j])) ++count;
                return (count == 2u || count > 3u);
            });
        }

        res_v double_sequence(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t count = 0u;
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_equivalent(d.melds()[i], d.melds()[j])) ++count;
                return (count >= 1u);
            });
        }
        
        res_v mixed_triple_triplet(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t count = 0u;
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_mixed_double_triplet(d.melds()[i], d.melds()[j])) ++count;
                return count == 3u;
            });
        }

        res_v mixed_double_triplet(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t count = 0u;
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_mixed_double_triplet(d.melds()[i], d.melds()[j])) ++count;
                return count >= 1u;
            });
        }

        res_v two_mixed_double_triplets(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t count = 0u;
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_mixed_double_triplet(d.melds()[i], d.melds()[j])) ++count;
                return count >= 2u;
            });
        }

        res_v mixed_triple_sequence(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (tile_t ti : {2_m, 3_m, 4_m, 5_m, 6_m, 7_m, 8_m})
                    if (utils::contains(d, {meld(ti, meld_type::sequence), meld(ti + 0b00100000u, meld_type::sequence), meld(ti + 0b10000000u, meld_type::sequence)})) 
                        return true;
                return false;
            });
        }

        res_v two_mixed_double_sequences(const hand& h) {
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

        res_v mixed_double_sequence(const hand& h) {
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

        res_v four_shifted_triplets(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (tile_t ti : {1_m, 2_m, 3_m, 4_m, 5_m, 6_m, 1_p, 2_p, 3_p, 4_p, 5_p, 6_p, 1_s, 2_s, 3_s, 4_s, 5_s, 6_s})
                    if (utils::contains(d, {meld(ti, meld_type::triplet), meld(ti + 1, meld_type::triplet), meld(ti + 2, meld_type::triplet), meld(ti + 3, meld_type::triplet)})) 
                        return true;
                return false;
            });
        }

        res_v three_shifted_triplets(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (tile_t ti : {1_m, 2_m, 3_m, 4_m, 5_m, 6_m, 7_m, 1_p, 2_p, 3_p, 4_p, 5_p, 6_p, 7_p, 1_s, 2_s, 3_s, 4_s, 5_s, 6_s, 7_s})
                    if (utils::contains(d, {meld(ti, meld_type::triplet), meld(ti + 1, meld_type::triplet), meld(ti + 2, meld_type::triplet)})) 
                        return true;
                return false;
            });
        }

        res_v four_shifted_sequences(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (tile_t ti : {2_m, 3_m, 4_m, 5_m, 2_p, 3_p, 4_p, 5_p, 2_s, 3_s, 4_s, 5_s})
                    if (utils::contains(d, {meld(ti, meld_type::sequence), meld(ti + 1, meld_type::sequence), meld(ti + 2, meld_type::sequence), meld(ti + 3, meld_type::sequence)})) 
                        return true;
                return false;
            });
        }

        res_v three_shifted_sequences(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (tile_t ti : {2_m, 3_m, 4_m, 5_m, 6_m, 2_p, 3_p, 4_p, 5_p, 6_p, 2_s, 3_s, 4_s, 5_s, 6_s, 7_s})
                    if (utils::contains(d, {meld(ti, meld_type::sequence), meld(ti + 1, meld_type::sequence), meld(ti + 2, meld_type::sequence)})) 
                        return true;
                return false;
            });
        }

        res_v four_chained_sequences(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (tile_t ti : {2_m, 2_p, 2_s})
                    if (utils::contains(d, {meld(ti, meld_type::sequence), meld(ti + 2, meld_type::sequence), meld(ti + 4, meld_type::sequence), meld(ti + 6, meld_type::sequence)})) 
                        return true;
                return false;
            });
        }

        res_v three_chained_sequences(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (tile_t ti : {2_m, 2_p, 2_s, 3_m, 3_p, 3_s, 4_m, 4_p, 4_s})
                    if (utils::contains(d, {meld(ti, meld_type::sequence), meld(ti + 2, meld_type::sequence), meld(ti + 4, meld_type::sequence)})) 
                        return true;
                return false;
            });
        }

        res_v pure_straight(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (tile_t ti : {2_m, 2_p, 2_s})
                    if (utils::contains(d, {meld(ti, meld_type::sequence), meld(ti + 3, meld_type::sequence), meld(ti + 6, meld_type::sequence)})) 
                        return true;
                return false;
            });
        }

        res_v two_short_straights(const hand& h) {
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

        res_v short_straight(const hand& h) {
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

        res_v two_terminal_sequences(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (uint8_t i = 0; i < d.melds().size(); ++i)
                    for (uint8_t j = i + 1; j < d.melds().size(); ++j)
                        if (utils::is_shifted_sequences(d.melds()[i], d.melds()[j], 6))
                            return true;
                return false;
            });
        }

        res_v mixed_shifted_triplets(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (auto& seq : patterns::mixed_shifted_triplets)
                    if (utils::contains(d, seq)) return true;
                return false;
            });
        }

        res_v mixed_shifted_sequences(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (auto& seq : patterns::mixed_shifted_sequences)
                    if (utils::contains(d, seq)) return true;
                return false;
            });
        }

        res_v mixed_chained_sequences(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (auto& seq : patterns::mixed_chained_sequences)
                    if (utils::contains(d, seq)) return true;
                return false;
            });
        }

        res_v mixed_straight(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                for (auto& seq : patterns::mixed_straight)
                    if (utils::contains(d, seq)) return true;
                return false;
            });
        }

        res_t seven_pairs(const hand& h) {
            return is_seven_pairs(h);
        }

        res_t big_seven_honours(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            return utils::contains_pair_of(h, tile_set::honour_tiles);
        }

        res_t four_wind_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            return utils::contains_pair_of(h, tile_set::wind_tiles);
        }

        res_t three_dragon_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            return utils::contains_pair_of(h, tile_set::dragon_tiles);
        }

        res_t six_dragon_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            return utils::count_pair_of(h, tile_set::dragon_tiles) == 6u;
        }

        res_t seven_shifted_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            for (tile_t ti : {1_m, 2_m, 3_m, 1_p, 2_p, 3_p, 1_s, 2_s, 3_s})
                if (utils::contains_pair_of(h, {
                    static_cast<tile_t>(ti), static_cast<tile_t>(ti + 1), static_cast<tile_t>(ti + 2), static_cast<tile_t>(ti + 3), 
                    static_cast<tile_t>(ti + 4), static_cast<tile_t>(ti + 5), static_cast<tile_t>(ti + 6)
                })) return true;
            return false;
        }

        res_t six_shifted_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            for (tile_t ti : {1_m, 2_m, 3_m, 4_m, 1_p, 2_p, 3_p, 4_p, 1_s, 2_s, 3_s, 4_s})
                if (utils::contains_pair_of(h, {
                    static_cast<tile_t>(ti), static_cast<tile_t>(ti + 1), static_cast<tile_t>(ti + 2), 
                    static_cast<tile_t>(ti + 3), static_cast<tile_t>(ti + 4), static_cast<tile_t>(ti + 5)
                })) return true;
            return false;
        }

        res_t five_shifted_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            for (tile_t ti : {1_m, 2_m, 3_m, 4_m, 5_m, 1_p, 2_p, 3_p, 4_p, 5_p, 1_s, 2_s, 3_s, 4_s, 5_s})
                if (utils::contains_pair_of(h, {
                    static_cast<tile_t>(ti), static_cast<tile_t>(ti + 1), static_cast<tile_t>(ti + 2), 
                    static_cast<tile_t>(ti + 3), static_cast<tile_t>(ti + 4)
                })) return true;
            return false;
        }

        res_t four_shifted_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            for (tile_t ti : {1_m, 2_m, 3_m, 4_m, 5_m, 6_m, 1_p, 2_p, 3_p, 4_p, 5_p, 6_p, 1_s, 2_s, 3_s, 4_s, 5_s, 6_s})
                if (utils::contains_pair_of(h, {
                    static_cast<tile_t>(ti), static_cast<tile_t>(ti + 1), static_cast<tile_t>(ti + 2), 
                    static_cast<tile_t>(ti + 3)
                })) return true;
            return false;
        }

        res_t reflected_pairs(const hand& h) {
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

        res_t reflected_pairs_2(const hand& h) {
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

        res_t two_triple_pairs(const hand& h) {
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

        res_t mixed_triple_pair(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            auto m = tile_literals::operator""_m;
            auto p = tile_literals::operator""_p;
            auto s = tile_literals::operator""_s;
            for (uint8_t i = 1; i <= 9; ++i) {
                if (utils::contains_pair_of(h, {m(i), p(i), s(i)})) return true;
            }
            return false;
        }

        res_t three_mirrored_pairs(const hand& h) {
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

        res_t two_mirrored_pairs(const hand& h) {
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

        res_t eight_hog(const hand& h) {
            uint8_t count = 0u;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti) == 4) ++count;
            for (const auto& m : h.melds())
                if (m.type() == meld_type::kong) --count;
            return count >= 2u;
        }

        res_t twelve_hog(const hand& h) {
            uint8_t count = 0u;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti) == 4) ++count;
            for (const auto& m : h.melds())
                if (m.type() == meld_type::kong) --count;
            return count == 3u;
        }

        res_t disabled(const hand&) {
            return false;
        }

        res_v reflected_hand(const hand& h) {
            for (tile_t ti : tile_set::honour_tiles)
                if (h.counter().count(ti)) return {false};
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                return utils::is_reflection(d, d, 2 * d.pair().num());
            }, reflected_pairs);
        }

        res_v reflected_hand_2(const hand& h) {
            for (tile_t ti : tile_set::honour_tiles)
                if (h.counter().count(ti)) return {false};
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                return utils::is_reflection_2(d, d, 2 * d.pair().num());
            }, reflected_pairs_2);
        }

        res_v mirrored_hand(const hand& h) {
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

        res_t three_shifted_hogs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            for (tile_t ti : tile_set::numbered_tiles) {
                if (tile(ti).num() > 7) continue;
                if (utils::count_pair_of(h, {tile(ti), tile(ti + 1), tile(ti + 2)}) == 6u) {
                    return true;
                }
            }
            return false;
        }

        res_t mixed_triple_hog(const hand& h) {
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

        res_t mixed_shifted_hogs(const hand& h) {
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

        res_t seven_wind_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            return utils::count_pair_of(h, tile_set::wind_tiles) == 7u;
        }

        res_v connected_numbers(const hand& h) {
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

        res_t mixed_one_number(const hand& h) {
            uint16_t num_table = 0u;
            for (tile_t ti : tile_set::numbered_tiles)
                if (h.counter().count(ti))
                    num_table |= (1u << tile(ti).num());
            return utils::popcount(num_table) <= 1u;
        }

        res_t trivial(const hand& /*unused_hand*/) {
            return true;
        }

        res_t double_pair(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti) == 4) return true;
            return false;
        }

        res_t two_double_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            uint8_t count = 0u;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti) == 4) ++count;
            return count >= 2u;
        }

        res_t three_double_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            uint8_t count = 0u;
            for (tile_t ti : tile_set::all_tiles)
                if (h.counter().count(ti) == 4) ++count;
            return count >= 3u;
        }

        res_v three_wind_triplets(const hand& h) {
            return utils::for_all_decompositions<res_t>(h, [](const hand::decomposition& d) {
                uint8_t count = 0u;
                for (tile_t ti : tile_set::wind_tiles)
                    if (utils::contains(d, {meld(ti, meld_type::triplet)})) 
                        ++count;
                return count >= 3;
            });
        }

        res_t six_wind_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            return utils::count_pair_of(h, tile_set::wind_tiles) >= 6;
        }

        res_t five_wind_pairs(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            return utils::count_pair_of(h, tile_set::wind_tiles) >= 5;
        }

        res_t four_wind_pairs_2(const hand& h) {
            if (!is_seven_pairs(h)) return false;
            return utils::count_pair_of(h, tile_set::wind_tiles) >= 4;
        }

        res_t thirteen_orphans(const hand& h) {
            const std::vector<tile_t> required_tiles = {
                1_m, 9_m, 1_p, 9_p, 1_s, 9_s, 1_z, 2_z, 3_z, 4_z, 5_z, 6_z, 7_z
            };
            for (tile_t ti : required_tiles) {
                if (h.counter().count(ti) == 0) return false;
            }
            return true;
        }

    }

}
