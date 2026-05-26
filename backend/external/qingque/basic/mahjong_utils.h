#pragma once

#include "mahjong.h"

#include <bitset>

namespace mahjong {
    
    namespace utils {

        inline uint8_t popcount(uint32_t n) {
            return std::bitset<32>(n).count();
        }

        inline tile_t reflect_by(tile_t ti, num_t ref) {
            const num_t new_num = ref - (ti & 0b00001111u);
            if (new_num == 0 || new_num > 9) return tile::invalid;
            return (ti & 0b11110000u) | new_num;
        }

        inline tile_t reflect_suit(tile_t ti, suit_type suit) {
            if ((ti & 0b11100000u) == suit) return ti;
            if (suit == suit_type::z) return ti; // no reflection for dragons
            suit_type other_suit = suit_type::z;
            for (suit_type s_ : {suit_type::m, suit_type::p, suit_type::s}) {
                if (s_ != suit && s_ != (tile(ti).suit())) {
                    other_suit = s_;
                    break;
                }
            }
            return (ti & 0b00001111u) | static_cast<tile_t>(other_suit);
        }

        inline meld_t reflect_by(meld_t m, num_t ref) {
            const num_t new_num = ref - (m & 0b0000000000001111u);
            if (new_num == 0 || new_num > 9) return meld::invalid;
            return (m & 0b1111111111110000u) | new_num;
        }

        inline meld_t reflect_suit(meld_t m, suit_type suit) {
            meld m_(m);
            if (m_.tile().suit() == suit_type::z) return m;
            if (m_.tile().suit() == suit) return m;
            suit_type other_suit = suit_type::z;
            for (suit_type s_ : {suit_type::m, suit_type::p, suit_type::s}) {
                if (s_ != suit && s_ != m_.tile().suit()) {
                    other_suit = s_;
                    break;
                }
            }
            return (m & 0b1111111100001111u) | static_cast<meld_t>(other_suit);
        }

        inline bool is_equivalent(meld_t m1, meld_t m2) {
            return (m1 & 0b0000000111111111u) == (m2 & 0b0000000111111111u);
        }

        inline bool is_mixed_double_triplet(meld_t m1, meld_t m2) {
            if (!(m1 & 0b0000000100000000u) || !(m2 & 0b0000000100000000u)) return false;
            if (!(m1 & 0b0000000001000000u) || !(m2 & 0b0000000001000000u)) return false;
            return (m1 & 0b0000000000001111u) == (m2 & 0b0000000000001111u);
        }

        inline bool is_mixed_double_sequence(meld_t m1, meld_t m2) {
            if ((m1 & 0b0000000100000000u) || (m2 & 0b0000000100000000u)) return false;
            if (is_equivalent(m1, m2)) return false;
            return (m1 & 0b0000000000001111u) == (m2 & 0b0000000000001111u);
        }

        inline bool is_shifted_sequences(meld_t m1, meld_t m2, num_t inc) {
            if ((m1 & 0b0000000100000000u) || (m2 & 0b0000000100000000u)) return false;
            return std::abs((int32_t)(m1 & 0b0000000011111111u) - (int32_t)(m2 & 0b0000000011111111u)) == inc;
        }

        inline bool is_reflection(const hand::decomposition& d1, const hand::decomposition& d2, num_t ref) {
            if (reflect_by(d1.pair(), ref) != d2.pair()) return false;
            uint8_t paired = 0;
            for (const meld& m1 : d1.melds())
                for (uint8_t i = 0; i < d2.melds().size(); ++i) {
                    if (paired & (1 << i)) continue;
                    if (is_equivalent(m1, reflect_by(d2.melds()[i], ref))) {
                        paired |= 1 << i;
                        break;
                    }
                }
            return paired == (1 << d2.melds().size()) - 1;
        }

        inline bool is_reflection_2(const hand::decomposition& d1, const hand::decomposition& d2, num_t ref) {
            if (reflect_by(d1.pair(), ref) != d2.pair()) return false;
            uint8_t paired = 0;
            const suit_type ref_suit = d1.pair().suit();
            for (const meld& m1 : d1.melds())
                for (uint8_t i = 0; i < d2.melds().size(); ++i) {
                    if (paired & (1 << i)) continue;
                    if (is_equivalent(m1, reflect_suit(reflect_by(d2.melds()[i], ref), ref_suit))) {
                        paired |= 1 << i;
                        break;
                    }
                }
            return paired == (1 << d2.melds().size()) - 1;
        }

        template<typename T> requires std::is_constructible_v<tile, typename std::decay_t<T>::value_type>
        inline bool contains(const hand::decomposition& d, T&& m) {
            for (const meld_t& m2 : m) {
                bool found = false;
                for (const meld& m1 : d.melds())
                    if (is_equivalent(m1, m2)) {
                        found = true;
                        break;
                    }
                if (!found) return false;
            }
            return true;
        }

        template<typename T> requires std::is_constructible_v<tile, T>
        inline bool contains(const hand::decomposition& d, std::initializer_list<T> m) {
            for (const auto& m2 : m) {
                bool found = false;
                for (const meld& m1 : d.melds())
                    if (is_equivalent(m1, m2)) {
                        found = true;
                        break;
                    }
                if (!found) return false;
            }
            return true;
        }

        template<typename T> requires std::is_constructible_v<tile, typename std::decay_t<T>::value_type>
        inline bool contains_pair_of(const hand& d, T&& ti) {
            for (auto ti2 : ti)
                if (d.counter().count(ti2) != 2 && d.counter().count(ti2) != 4) return false;
            return true;
        }

        template<typename T> requires std::is_constructible_v<tile, T>
        inline bool contains_pair_of(const hand& d, std::initializer_list<T> ti) {
            for (auto ti2 : ti)
                if (d.counter().count(ti2) != 2 && d.counter().count(ti2) != 4) return false;
            return true;
        }

        template<typename T> requires std::is_constructible_v<tile, typename std::decay_t<T>::value_type>
        inline int count_pair_of(const hand& d, T&& ti) {
            int count = 0;
            for (auto ti2 : ti) {
                if (d.counter().count(ti2) == 2) ++count;
                if (d.counter().count(ti2) == 4) count += 2;
            }
            return count;
        }

        template<typename T> requires std::is_constructible_v<tile, T>
        inline int count_pair_of(const hand& d, std::initializer_list<T> ti) {
            int count = 0;
            for (auto ti2 : ti) {
                if (d.counter().count(ti2) == 2) ++count;
                if (d.counter().count(ti2) == 4) count += 2;
            }
            return count;
        }

        inline std::vector<tile_t> all_waits(const hand& h, const verifier& v) {
            std::vector<tile_t> waits;
            const tile_counter counter = h.counter(false) - h.winning_tile();
            auto vt = counter.tiles();
            for (tile_t ti : tile_set::all_tiles) {
                auto h2 = hand(vt, h.melds(), ti);
                if (v(h2)) waits.push_back(ti);
            }
            return waits;
        }

    }

}
