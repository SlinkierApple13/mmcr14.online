#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <sstream>
#include <string>
#include <vector>
#include "hand.h"

namespace mahjong {

    namespace utils {

        template<typename T> requires std::is_default_constructible_v<T>
        std::vector<T> for_all_decompositions(const hand& h, const std::function<T(const hand::decomposition&)>& f, const std::function<T(const hand&)>& g = nullptr) {
            std::vector<T> results;
            if (g) results.push_back(g(h));
            else results.push_back(T());
            for (const hand::decomposition& d : h.decompose())
            results.push_back(f(d));
            return results;
        }

        template<typename T> requires std::is_default_constructible_v<T>
        std::vector<T> for_all_decompositions(const hand& h, const std::function<T(const hand::decomposition&, const hand&)>& f, const std::function<T(const hand&)>& g = nullptr) {
            std::vector<T> results;
            if (g) results.push_back(g(h));
            else results.push_back(T());
            for (const hand::decomposition& d : h.decompose())
            results.push_back(f(d, h));
            return results;
        }

        inline tile_t parse_tile(const std::string& str) {
            try {
                switch (str.back()) {
                    case 'm':
                        return tile_set::character_tiles[str[0] - '1'];
                    case 'p':
                        return tile_set::dot_tiles[str[0] - '1'];
                    case 's':
                        return tile_set::bamboo_tiles[str[0] - '1'];
                    case 'z':
                        return std::basic_string<tile_t>{honours::E, honours::S, honours::W, honours::N, honours::P, honours::F, honours::C}[str[0] - '1'];
                    case 'E':
                        return honours::E;
                    case 'S':
                        return honours::S;
                    case 'W':
                        return honours::W;
                    case 'N':
                        return honours::N;
                    case 'C':
                        return honours::C;
                    case 'F':
                        return honours::F;
                    case 'P':
                        return honours::P;
                    default:
                        return tile::invalid;
                }
            } catch (...) {
                return tile::invalid;
            }
        }

        inline meld_t parse_meld(const std::string& str) {
            if (str.size() < 5) return meld::invalid;
            std::string substr = str.substr(1, str.size() - 2);
            tile_t ti = parse_tile(substr);
            if (!ti) return meld::invalid;
            std::string triplet_case = meld(ti, meld_type::triplet, false, true);
            std::string kong_case = meld(ti, meld_type::kong, false, true);
            if (str == '(' + triplet_case + ')') return meld(ti, meld_type::triplet, false, true);
            if (str == '(' + kong_case + ')') return meld(ti, meld_type::kong, false, true);
            if (str == '[' + kong_case + ']') return meld(ti, meld_type::kong, true, true);
            if (str.size() != 6) return meld::invalid;
            if (str[0] != '(' || str[5] != ')') return meld::invalid;
            std::array<int, 3> nums = {str[1] - '0', str[2] - '0', str[3] - '0'};
            std::sort(nums.begin(), nums.end());
            if (nums[0] + 1 != nums[1] || nums[1] + 1 != nums[2]) return meld::invalid;
            return meld((ti & 0b11110000) | nums[1], meld_type::sequence, false, true);
        }

        inline hand parse_hand(const std::string& str, tile_t winning_tile = tile::invalid, win_t win_type = 0u, bool winning_tile_included = false, std::function<bool(char, win_t&)> win_type_parser = nullptr, bool knitted_straight = knitted_straight_default) {
            std::stringstream ss(str);
            std::vector<tile_t> tiles;
            std::vector<meld> open_melds;
            std::string buf;
            char c;
            char in_meld = 0;
            while (ss >> c) {
                if (c == '(' || c == '[') {
                    if (in_meld)
                        return hand({}, {}, 0u, 0u, true);
                    in_meld = c;
                    buf = c;
                    continue;
                }
                if (c == ')' || c == ']') {
                    if (!in_meld || (in_meld == '(' && c == ']') || (in_meld == '[' && c == ')'))
                        return hand({}, {}, 0u, 0u, true);
                    in_meld = 0;
                    buf += c;
                    meld_t m = parse_meld(buf);
                    if (m == meld::invalid) return hand({}, {}, 0u, 0u, true);
                    open_melds.push_back((meld)m);
                    buf = "";
                    continue;
                }
                if (in_meld) {
                    buf += c;
                    continue;
                }
                if (c == 'E' || c == 'S' || c == 'W' || c == 'N' || c == 'C' || c == 'F' || c == 'P') {
                    if (buf.size()) return hand({}, {}, 0u, 0u, true);
                    tiles.push_back(parse_tile({c}));
                    continue;
                }
                if (c == 'm' || c == 'p' || c == 's') {
                    if (buf.size() == 0) return hand({}, {}, 0u, 0u, true);
                    for (char d : buf) {
                        tile_t ti = parse_tile({d, c});
                        if (!ti) return hand({}, {}, 0u, 0u, true);
                        tiles.push_back(ti);
                    }
                    buf = "";
                    continue;
                }
                if (c == ' ')
                    continue;
                if (win_type_parser != nullptr && win_type_parser(c, win_type)) continue;
                buf += c;
            }
            if (buf.size() || tiles.empty()) return hand({}, {}, 0u, 0u, true);
            if (!winning_tile) {
                winning_tile_included = true;
                winning_tile = tiles.back();
            }
            if (tiles.size() + 3 * open_melds.size() != 14) return hand({}, {}, 0u, 0u, true);
            auto h = hand(tiles, open_melds, winning_tile, win_type, winning_tile_included, knitted_straight);
            return h.is_valid() ? h : hand({}, {}, 0u, 0u, true);
        }

    }

}
