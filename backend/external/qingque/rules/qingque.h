#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "../basic/mahjong.h"
#include "../basic/mahjong_utils.h"
#include "qingque_data.h"

namespace qingque {

    namespace patterns {
        extern unsigned long long nine_gates_m_s;
        extern unsigned long long nine_gates_p;

        extern const std::array<std::array<mahjong::meld_t, 3>, 42> mixed_shifted_triplets;
        extern const std::array<std::array<mahjong::meld_t, 3>, 30> mixed_shifted_sequences;
        extern const std::array<std::array<mahjong::meld_t, 3>, 18> mixed_chained_sequences;
        extern const std::array<std::array<mahjong::meld_t, 3>, 6> mixed_straight;
        extern const std::array<std::array<mahjong::meld_t, 4>, 15> mirrored_short_straights;

        extern unsigned long long knitted_tiles;
        extern unsigned long long honours;

        extern const std::array<mahjong::tile_counter, 6> honours_and_knitted_tiles;
    }

    struct tag {
        bool special_compatible = false;
        bool is_special = false;
        bool is_occasional = false;
        uint8_t fan_value = 0u;

        tag(bool special_compatible = false, bool is_special = false, bool is_occasional = false, uint8_t fan_value = 0u) :
            special_compatible(special_compatible), is_special(is_special), is_occasional(is_occasional), fan_value(fan_value) {}
    };

    using fan = mahjong::scoring_element<uint8_t, tag>;
    using fan_code = std::bitset<code_size>;

    extern mahjong::verifier is_seven_pairs;
    extern mahjong::verifier is_thirteen_orphans;
    extern mahjong::verifier input_verifier;
    extern mahjong::verifier is_winning_hand;

    extern const std::vector<fan> fans;

    enum indices {
        trivial,
        heavenly_hand,
        earthly_hand,
        out_with_replacement_tile,
        last_tile_draw,
        last_tile_claim,
        robbing_the_kong,
        thirteen_orphans,
        seven_pairs,
        concealed_hand,
        four_concealed_kongs,
        three_concealed_kongs,
        two_concealed_kongs,
        concealed_kong,
        four_kongs,
        three_kongs,
        two_kongs,
        four_concealed_triplets,
        three_concealed_triplets,
        all_triplets,
        twelve_hog,
        eight_hog,
        three_double_pairs,
        two_double_pairs,
        double_pair,
        all_honours,
        big_four_winds,
        little_four_winds,
        four_wind_pairs,
        three_wind_triplets,
        seven_wind_pairs,
        six_wind_pairs,
        big_three_dragons,
        little_three_dragons,
        six_dragon_pairs,
        three_dragon_pairs,
        fan_tile_4t,
        fan_tile_3t,
        fan_tile_2t,
        fan_tile_1t,
        fan_tile_7p,
        fan_tile_6p,
        fan_tile_5p,
        fan_tile_4p,
        fan_tile_3p,
        fan_tile_2p,
        fan_tile_1p,
        all_terminals,
        all_terminals_and_honours,
        pure_outside_hand,
        mixed_outside_hand,
        nine_gates,
        full_flush,
        half_flush,
        all_types,
        mixed_one_number,
        two_numbers,
        two_consecutive_numbers,
        three_consecutive_numbers,
        four_consecutive_numbers,
        connected_numbers,
        gapped_numbers,
        reflected_hand,
        reflected_hand_2,
        common_number,
        quadruple_sequence,
        triple_sequence,
        two_double_sequences,
        double_sequence,
        four_shifted_triplets,
        three_shifted_triplets,
        four_shifted_sequences,
        three_shifted_sequences,
        four_chained_sequences,
        three_chained_sequences,
        pure_straight,
        seven_shifted_pairs,
        six_shifted_pairs,
        five_shifted_pairs,
        four_shifted_pairs,
        mixed_triple_triplet,
        mixed_triple_sequence,
        two_triple_pairs,
        mixed_triple_pair,
        mixed_shifted_triplets,
        mixed_straight,
        mirrored_hand,
        three_mirrored_pairs,
        two_mirrored_pairs,
        two_short_straights
    };

    std::vector<fan_code> evaluate_fans(const mahjong::hand& h, bool ignore_occ = false);
    unsigned long long get_weight(const w_data& data, const fan_code& res);
    std::pair<unsigned long long, fan_code> get_weight(const w_data& data, const mahjong::hand& h);
    double get_fan(const w_data& data, const fan_code& res);
    std::pair<double, fan_code> get_fan(const w_data& data, const mahjong::hand& h);
    fan_code dedupe(const fan_code& res);
    fan_code dedupe2(const fan_code& res);
    std::vector<fan_code> dedupe(const std::vector<fan_code>& res);
    std::vector<fan_code> dedupe2(const std::vector<fan_code>& res);
    bool has_fan(const mahjong::hand& h);
    std::unordered_map<mahjong::tile_t, std::unordered_map<mahjong::tile_t, std::pair<double, double>>> get_all_waits(const w_data& data, const mahjong::hand& h);

} // namespace qingque