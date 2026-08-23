#pragma once

#include <bitset>
#include <queue>
#include <utility>
#include <vector>
#include "tiles.h"

namespace mahjong {

    class hand {

    private:
        win_t win_type;
        tile_t win_tile;
        std::vector<meld> open_melds;
        
        tile_counter closed_counter;
        tile_counter total_counter;
        bool knitted_straight_allowed;

    public:
        // win_type(16):win_tile(16):open_meld_0(16):open_meld_1(16):open_meld_2(16):open_meld_3(16):closed_counter_s_z(64):closed_counter_m_p(64)
        using hand_t = std::bitset<320>;
        
        inline hand_t to_bits() const {
            hand_t bits;
            bits |= win_type;
            bits <<= 16;
            bits |= win_tile;
            for (const meld& m : open_melds) {
                bits <<= 16;
                bits |= (meld_t)m;
            }
            for (int i = open_melds.size(); i < 4; ++i) {
                bits <<= 16;
            }
            bits <<= 64;
            bits |= closed_counter[1];
            bits <<= 64;
            bits |= closed_counter[0];
            return bits;
        }

        static inline hand from_bits(hand_t bits) {
            const hand_t mask16 = 0xffff;
            const hand_t mask64 = 0xffffffffffffffff;
            unsigned long long m_p = (bits & mask64).to_ullong();
            bits >>= 64;
            unsigned long long s_z = (bits & mask64).to_ullong();
            bits >>= 64;
            auto tiles = tile_counter(m_p, s_z).tiles();
            std::vector<meld> reversed_melds;
            for (uint8_t i = 0; i < 4; ++i) {
                meld_t m = (meld_t)(bits & mask16).to_ullong();
                if (m) reversed_melds.push_back(meld(m));
                bits >>= 16;
            }
            std::vector<meld> melds(reversed_melds.rbegin(), reversed_melds.rend());
            tile_t wtile = (tile_t)(bits & mask16).to_ullong();
            bits >>= 16;
            win_t wtype = (win_t)(bits & mask16).to_ullong();
            return hand(tiles, melds, wtile, wtype, true);
        }

        class decomposition {
        
        private:
            const hand& h;
            const tile pair_tile;
            std::vector<meld> all_melds;
            tile_counter remaining_counter;

        public:
            inline const hand& original_hand() const {
                return h;
            }

            inline const tile_counter& counter(bool include_open_melds = true) const {
                if (remaining_counter) return remaining_counter;
                return h.counter(include_open_melds);
            }

            inline win_t winning_type() const {
                return h.win_type;
            }

            inline tile winning_tile() const {
                return h.win_tile;
            }

            inline bool is_won_by(win_t type, bool inverse = false) const {
                return ((h.win_type & type) == type) ^ inverse;
            }

            inline const std::vector<meld>& melds() const {
                return all_melds;
            }

            inline std::vector<meld>& melds() {
                return all_melds;
            }

            inline tile pair() const {
                return pair_tile;
            }

            decomposition(const hand& h): h(h), pair_tile(0u), all_melds(h.open_melds), remaining_counter(h.closed_counter) {}
            decomposition(const decomposition& d, const meld& m) : h(d.h), pair_tile(d.pair_tile), all_melds(d.all_melds), remaining_counter(d.remaining_counter - m) {
                all_melds.push_back(m);
            }
            decomposition(const decomposition& d, tile_t pair_tile) : h(d.h), pair_tile(pair_tile), all_melds(d.all_melds), remaining_counter() {}
            decomposition(const hand& h, tile_t pair_tile, std::vector<meld> all_melds) : h(h), pair_tile(pair_tile), all_melds(all_melds), remaining_counter() {}

            friend std::ostream& operator<<(std::ostream& os, const decomposition& d) {
                for (const meld& m : d.melds())
                    os << m << ' ';
                if (d.pair().num() != 0) os << (int)d.pair().num() << d.pair();
                else os << d.pair() << d.pair();
                return os;
            }

#ifdef MAHJONG_KNITTED_STRAIGHT
            inline bool is_knitted_straight() const {
                return all_melds.size() == 1;
            }
#endif
        
        };

    private:
        mutable std::vector<decomposition> decompositions;
        mutable bool decomposed = false;

        inline void decompose_init() const {
            decompositions.clear();
            std::queue<std::pair<decomposition, uint8_t>> queue({std::make_pair(decomposition(*this), 0u)});
            while (!queue.empty()) {
                const decomposition& front = queue.front().first;
                const uint8_t index = queue.front().second;
                const tile_counter& counter = front.counter();
                if (front.melds().size() == 4) {
                    for (tile_t ti : tile_set::all_tiles)
                        if (counter.count(ti) == 2) {
                            decompositions.push_back(decomposition(front, ti));
                            break;
                        }
                    queue.pop();
                    continue;
                }

#ifdef MAHJONG_KNITTED_STRAIGHT
                if (knitted_straight_allowed && front.melds().size() == 1) {
                    using namespace patterns;
                    for (uint8_t i = 0; i < 6; ++i) {
                        if (!(counter >= knitted_straight_counters[i])) continue;
                        const tile_counter c = counter - knitted_straight_counters[i];
                        for (tile_t ti : tile_set::all_tiles)
                            if (c.count(ti) == 2) {
                                decompositions.push_back(decomposition(front, ti));
                                break;
                            }
                    }
                }
#endif

                for (uint8_t i = (index + 1) >> 1; i < 34; ++i) {
                    tile_t ti = tile_set::all_tiles[i];
                    if (counter.count(ti) >= 3)
                        queue.push({decomposition(front, meld(ti, meld_type::triplet, (mahjong::win_type(this->win_type)(mahjong::win_type::self_drawn) || ti != win_tile || closed_counter.count(ti) == 4), false)), i * 2});
                }
                for (uint8_t i = index >> 1; i < 27; ++i) {
                    tile_t ti = tile_set::numbered_tiles[i];
                    if (counter.count(ti) && counter.count(ti + 1) && counter.count(ti - 1))
                        queue.push({decomposition(front, meld(ti, meld_type::sequence, false, false)), i * 2 + 1});
                }
                queue.pop();
            }
        }

    public:
        template<typename F> requires std::is_constructible_v<tile, F>
        hand(const std::vector<F>& tiles, const std::vector<meld>& melds, F wtile, win_t wtype = 0u, bool winning_tile_included = false, bool knitted_straight = knitted_straight_default) : 
            win_type(wtype), win_tile(wtile), open_melds(melds), closed_counter(tiles), total_counter(tiles, melds), knitted_straight_allowed(knitted_straight) {
            if (!winning_tile_included) {
                closed_counter.add(win_tile);
                total_counter.add(win_tile);
            }
        }

        template<typename F> requires std::is_constructible_v<tile, F>
        hand(std::vector<F>&& tiles, std::vector<meld>&& melds, F wtile, win_t wtype = 0u, bool winning_tile_included = false, bool knitted_straight = knitted_straight_default) : 
            win_type(wtype), win_tile(wtile), open_melds(melds), closed_counter(tiles), total_counter(tiles, melds), knitted_straight_allowed(knitted_straight) {
            if (!winning_tile_included) {
                closed_counter.add(win_tile);
                total_counter.add(win_tile);
            }
        }

        template<typename F> requires std::is_constructible_v<tile, F>
        hand(const std::vector<meld>& all_melds, F pair, F wtile, win_t wtype = 0u, bool knitted_straight = knitted_straight_default) : 
            win_type(wtype), win_tile(wtile), open_melds(), closed_counter({pair, pair}), total_counter({pair, pair}, all_melds), knitted_straight_allowed(knitted_straight) {
            for (const meld& m : all_melds) {
                if (m.fixed()) open_melds.push_back(m);
                else closed_counter.add(m);
            }
        }

        template<typename F> requires std::is_constructible_v<tile, F>
        hand(std::vector<meld>&& all_melds, F pair, F wtile, win_t wtype = 0u, bool knitted_straight = knitted_straight_default) : 
            win_type(wtype), win_tile(wtile), open_melds(), closed_counter({pair, pair}), total_counter({pair, pair}, all_melds), knitted_straight_allowed(knitted_straight) {
            for (const meld& m : all_melds) {
                if (m.fixed()) open_melds.push_back(m);
                else closed_counter.add(m);
            }
        }

        hand(const hand& h) : win_type(h.win_type), win_tile(h.win_tile), open_melds(h.open_melds), closed_counter(h.closed_counter), total_counter(h.total_counter), knitted_straight_allowed(h.knitted_straight_allowed), decomposed(h.decomposed) {
            if (!decomposed) return;
            for (const decomposition& d : h.decompositions)
                decompositions.push_back(decomposition(*this, d.pair(), d.melds()));
        }

        inline hand& operator=(const hand& h) {
            if (this == &h) return *this;
            win_type = h.win_type;
            win_tile = h.win_tile;
            open_melds = h.open_melds;
            closed_counter = h.closed_counter;
            total_counter = h.total_counter;
            knitted_straight_allowed = h.knitted_straight_allowed;
            decomposed = h.decomposed;
            decompositions.clear();
            if (!decomposed) return *this;
            for (const decomposition& d : h.decompositions)
                decompositions.push_back(decomposition(*this, d.pair(), d.melds()));
            return *this;
        }

        inline mahjong::win_type winning_type() const {
            return win_type;
        }

        inline void set_winning_type(win_t type) {
            win_type = type;
            for (auto& d : decompositions) {
                for (auto& m : d.melds()) {
                    if (m.type() != meld_type::triplet) continue;
                    if (m.fixed()) continue;
                    if (mahjong::win_type(type)(mahjong::win_type::self_drawn) || m.tile() != win_tile || closed_counter.count(m.tile()) == 4)
                        m = meld(m.tile(), meld_type::triplet, true, false);
                    else
                        m = meld(m.tile(), meld_type::triplet, false, false);
                }
            }
        }

        inline tile winning_tile() const {
            return win_tile;
        }

        inline const std::vector<meld>& melds() const {
            return open_melds;
        }

        inline const std::vector<decomposition>& decompose() const {
            if (!decomposed) {
                decompose_init();
                decomposed = true;
            }
            return decompositions;
        }

        inline const tile_counter& counter(bool include_open_melds = true) const {
            return include_open_melds ? total_counter : closed_counter;
        }

        inline bool is_valid(bool check_fifth_tile = true) const {
            if (!((closed_counter.count() + open_melds.size() * 3 == 14) && closed_counter.count(win_tile)))
                return false;
            uint8_t kong_count = 0;
            for (const meld& m : open_melds)
                if (m.type() == meld_type::kong) ++kong_count;
            if (total_counter.count() - kong_count != 14) return false;
            if (!check_fifth_tile) return true;
            for (tile_t ti : tile_set::all_tiles)
                if (total_counter.count(ti) > 4)
                    return false;
            return true;
        }

        friend std::ostream& operator<<(std::ostream& os, const hand& h) {
            for (const meld& m : h.melds())
                os << m << ' ';
            std::string s1 = "", s2 = "", s3 = "";
            for (tile_t ti : tile_set::character_tiles)
                for (uint8_t i = 0; i < h.counter(false).count(ti) - (ti == h.win_tile); ++i)
                    s1 += mahjong::tile(ti).num() + '0';
            if (s1.size()) s1 += 'm';
            for (tile_t ti : tile_set::dot_tiles)
                for (uint8_t i = 0; i < h.counter(false).count(ti) - (ti == h.win_tile); ++i)
                    s2 += mahjong::tile(ti).num() + '0';
            if (s2.size()) s2 += 'p';
            for (tile_t ti : tile_set::bamboo_tiles)
                for (uint8_t i = 0; i < h.counter(false).count(ti) - (ti == h.win_tile); ++i)
                    s3 += mahjong::tile(ti).num() + '0';
            if (s3.size()) s3 += 's';
            os << s1 << s2 << s3;
            for (tile_t ti: tile_set::honour_tiles)
                for (uint8_t i = 0; i < h.counter(false).count(ti) - (ti == h.win_tile); ++i)
                    os << mahjong::tile(ti);
            os << ' ' << mahjong::tile(h.win_tile);
            os << ' ' << std::bitset<16>(h.win_type);
            return os;
        }

    };

}
