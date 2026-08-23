#include "../basic/mahjong.h"
#include "../basic/mahjong_utils.h"
#include "qingque.h"

namespace qingque {

    using namespace mahjong;

    struct Cover {
        const fan_code& res;
        fan_code& new_res;
        Cover(const fan_code& res, fan_code& new_res) : res(res), new_res(new_res) {}

        template<typename... Indices>
        void operator()(qingque::indices i, Indices... j) {
            static_cast<void>(i);
            ((new_res[j] = new_res[j] && !res[i]), ...);
        }
    };

    // Simplify fan structure while preserving the meaning of the fan result. 
    // For example, {fan_tile_2p, fan_tile_1p} -> {fan_tile_2p}
    fan_code dedupe(const fan_code& res) {
        fan_code new_res = res;
        using enum indices;
        Cover cover(res, new_res);

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
    fan_code dedupe2(const fan_code& res) {
        fan_code new_res = res;
        using enum indices;
        Cover cover(res, new_res);

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

    std::vector<fan_code> dedupe(const std::vector<fan_code>& res) {
        std::vector<fan_code> new_res;
        for (const auto& r : res) {
            new_res.push_back(dedupe(r));
        }
        return new_res;
    }

    std::vector<fan_code> dedupe2(const std::vector<fan_code>& res) {
        std::vector<fan_code> new_res;
        for (const auto& r : res) {
            new_res.push_back(dedupe2(r));
        }
        return new_res;
    }

}
