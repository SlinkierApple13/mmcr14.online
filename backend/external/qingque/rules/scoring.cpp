#include <cmath>
#include <iostream>
#include <utility>

#include "../basic/mahjong.h"
#include "../basic/mahjong_utils.h"
#include "qingque.h"
#include "criteria.h"

namespace qingque {

    using namespace mahjong;

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

}
