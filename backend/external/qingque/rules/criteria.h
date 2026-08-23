#pragma once

#include <cstdint>
#include <vector>
#include "../basic/mahjong.h"

namespace qingque {

    namespace criteria {

        using res_t = uint8_t;
        using res_v = std::vector<uint8_t>;

        res_t heavenly_hand(const mahjong::hand& h);
        res_t earthly_hand(const mahjong::hand& h);
        res_t out_with_replacement_tile(const mahjong::hand& h);
        res_t last_tile_draw(const mahjong::hand& h);
        res_t last_tile_claim(const mahjong::hand& h);
        res_t robbing_the_kong(const mahjong::hand& h);
        res_t self_drawn(const mahjong::hand& h);
        res_t concealed_hand(const mahjong::hand& h);
        res_v four_concealed_kongs(const mahjong::hand& h);
        res_v three_concealed_kongs(const mahjong::hand& h);
        res_v two_concealed_kongs(const mahjong::hand& h);
        res_v concealed_kong(const mahjong::hand& h);
        res_t four_kongs(const mahjong::hand& h);
        res_t three_kongs(const mahjong::hand& h);
        res_t two_kongs(const mahjong::hand& h);
        res_t kong(const mahjong::hand& h);
        res_v four_concealed_triplets(const mahjong::hand& h);
        res_v three_concealed_triplets(const mahjong::hand& h);
        res_v two_concealed_triplets(const mahjong::hand& h);
        res_v concealed_triplet(const mahjong::hand& h);
        res_v all_triplets(const mahjong::hand& h);
        res_t all_honours(const mahjong::hand& h);
        res_t big_four_winds(const mahjong::hand& h);
        res_t little_four_winds(const mahjong::hand& h);
        res_t big_three_dragons(const mahjong::hand& h);
        res_t little_three_dragons(const mahjong::hand& h);
        res_t fan_tile_1p(const mahjong::hand& h);
        res_v fan_tile_2p(const mahjong::hand& h);
        res_v fan_tile_3p(const mahjong::hand& h);
        res_v fan_tile_4p(const mahjong::hand& h);
        res_t fan_tile_5p(const mahjong::hand& h);
        res_t fan_tile_6p(const mahjong::hand& h);
        res_t fan_tile_7p(const mahjong::hand& h);
        res_t fan_tile_1t(const mahjong::hand& h);
        res_t fan_tile_2t(const mahjong::hand& h);
        res_t fan_tile_3t(const mahjong::hand& h);
        res_t fan_tile_4t(const mahjong::hand& h);
        res_t all_terminals(const mahjong::hand& h);
        res_t all_terminals_and_honours(const mahjong::hand& h);
        res_v pure_outside_hand(const mahjong::hand& h);
        res_v mixed_outside_hand(const mahjong::hand& h);
        res_t nine_gates(const mahjong::hand& h);
        res_t full_flush(const mahjong::hand& h);
        res_t half_flush(const mahjong::hand& h);
        res_t all_types(const mahjong::hand& h);
        res_t two_numbers(const mahjong::hand& h);
        res_t two_consecutive_numbers(const mahjong::hand& h);
        res_t three_consecutive_numbers(const mahjong::hand& h);
        res_t four_consecutive_numbers(const mahjong::hand& h);
        res_t gapped_numbers(const mahjong::hand& h);
        res_v nine_numbers(const mahjong::hand& h);
        res_v common_number(const mahjong::hand& h);
        res_v quadruple_sequence(const mahjong::hand& h);
        res_v triple_sequence(const mahjong::hand& h);
        res_v two_double_sequences(const mahjong::hand& h);
        res_v double_sequence(const mahjong::hand& h);
        res_v mixed_triple_triplet(const mahjong::hand& h);
        res_v mixed_double_triplet(const mahjong::hand& h);
        res_v two_mixed_double_triplets(const mahjong::hand& h);
        res_v mixed_triple_sequence(const mahjong::hand& h);
        res_v two_mixed_double_sequences(const mahjong::hand& h);
        res_v mixed_double_sequence(const mahjong::hand& h);
        res_v four_shifted_triplets(const mahjong::hand& h);
        res_v three_shifted_triplets(const mahjong::hand& h);
        res_v four_shifted_sequences(const mahjong::hand& h);
        res_v three_shifted_sequences(const mahjong::hand& h);
        res_v four_chained_sequences(const mahjong::hand& h);
        res_v three_chained_sequences(const mahjong::hand& h);
        res_v pure_straight(const mahjong::hand& h);
        res_v two_short_straights(const mahjong::hand& h);
        res_v short_straight(const mahjong::hand& h);
        res_v two_terminal_sequences(const mahjong::hand& h);
        res_v mixed_shifted_triplets(const mahjong::hand& h);
        res_v mixed_shifted_sequences(const mahjong::hand& h);
        res_v mixed_chained_sequences(const mahjong::hand& h);
        res_v mixed_straight(const mahjong::hand& h);
        res_t seven_pairs(const mahjong::hand& h);
        res_t big_seven_honours(const mahjong::hand& h);
        res_t four_wind_pairs(const mahjong::hand& h);
        res_t three_dragon_pairs(const mahjong::hand& h);
        res_t six_dragon_pairs(const mahjong::hand& h);
        res_t seven_shifted_pairs(const mahjong::hand& h);
        res_t six_shifted_pairs(const mahjong::hand& h);
        res_t five_shifted_pairs(const mahjong::hand& h);
        res_t four_shifted_pairs(const mahjong::hand& h);
        res_t reflected_pairs(const mahjong::hand& h);
        res_t reflected_pairs_2(const mahjong::hand& h);
        res_t two_triple_pairs(const mahjong::hand& h);
        res_t mixed_triple_pair(const mahjong::hand& h);
        res_t three_mirrored_pairs(const mahjong::hand& h);
        res_t two_mirrored_pairs(const mahjong::hand& h);
        res_t eight_hog(const mahjong::hand& h);
        res_t twelve_hog(const mahjong::hand& h);
        res_t disabled(const mahjong::hand&);
        res_v reflected_hand(const mahjong::hand& h);
        res_v reflected_hand_2(const mahjong::hand& h);
        res_v mirrored_hand(const mahjong::hand& h);
        res_t three_shifted_hogs(const mahjong::hand& h);
        res_t mixed_triple_hog(const mahjong::hand& h);
        res_t mixed_shifted_hogs(const mahjong::hand& h);
        res_t seven_wind_pairs(const mahjong::hand& h);
        res_v connected_numbers(const mahjong::hand& h);
        res_t mixed_one_number(const mahjong::hand& h);
        res_t trivial(const mahjong::hand& /*unused_hand*/);
        res_t double_pair(const mahjong::hand& h);
        res_t two_double_pairs(const mahjong::hand& h);
        res_t three_double_pairs(const mahjong::hand& h);
        res_v three_wind_triplets(const mahjong::hand& h);
        res_t six_wind_pairs(const mahjong::hand& h);
        res_t five_wind_pairs(const mahjong::hand& h);
        res_t four_wind_pairs_2(const mahjong::hand& h);
        res_t thirteen_orphans(const mahjong::hand& h);

    } // namespace criteria

} // namespace qingque
