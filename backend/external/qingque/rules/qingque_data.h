#pragma once

#include <bitset>
#include <unordered_map>
#include <stdint.h>
#include <vector>

namespace qingque {

    constexpr unsigned code_size = 96;
    using fan_code = std::bitset<code_size>;

    struct fan_r_vec_hash {
        std::size_t operator()(const std::vector<std::bitset<qingque::code_size>>& v) const noexcept {
            std::size_t hash = 0;
            for (const auto& res : v) {
                hash ^= std::hash<std::bitset<qingque::code_size>>{}(res);
            }
            return hash;
        }
    };

    struct w_data {
        unsigned long long total_weight;
        std::unordered_map<std::vector<std::bitset<code_size>>, unsigned long long, fan_r_vec_hash> weights;
        std::unordered_map<int, int> a_table; // fan_id -> 20a
        std::unordered_map<std::bitset<code_size>, double> fan_cache;
        w_data() : total_weight(0) {}

        int get_a(int fan_id) const;
    };
    
}