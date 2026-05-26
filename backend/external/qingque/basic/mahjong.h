#pragma once

#include <assert.h>
#include <stdint.h>
#include <vector>
#include <string>
#include <array>
#include <queue>
#include <algorithm>
#include <functional>
#include <sstream>
#include <bitset>

namespace mahjong {

    typedef uint8_t tile_t;
    typedef uint8_t num_t;
    typedef uint8_t hand_type_t;
    typedef uint16_t meld_t;
    typedef uint16_t win_t;

    enum suit_type : tile_t {
        m = 0b01000000u,
        p = 0b01100000u,
        s = 0b11000000u,
        z = 0b10100000u,
        d = 0b10110000u
    };

    namespace honours {
        constexpr tile_t E = 0b10100001u;
        constexpr tile_t S = 0b10100010u;
        constexpr tile_t W = 0b10100011u;
        constexpr tile_t N = 0b10100100u;
        constexpr tile_t C = 0b10110101u;
        constexpr tile_t F = 0b10110110u;
        constexpr tile_t P = 0b10110111u;
    }

    enum meld_type : meld_t {
        sequence = 0b0000000000u, 
        triplet = 0b0100000000u, 
        kong = 0b1100000000u
    };

    class win_type {
    private:
        win_t t;
    
    public:
        constexpr static win_t self_drawn = 0b00010000u;
        constexpr static win_t final_tile = 0b00100000u;
        constexpr static win_t kong_related = 0b01000000u;
        constexpr static win_t heavenly_or_earthly_hand = 0b10000000u;
        constexpr static win_t seat_wind_filter = 0b00000011u;
        constexpr static win_t prevalent_wind_filter = 0b00001100u;

        inline bool operator()(const win_t is, const win_t is_not = 0u) const {
            return ((t & is) == is) && ((t & is_not) == 0);
        }

        inline tile_t seat_wind() const {
            return suit_type::z + 1u + (t & seat_wind_filter);
        }

        inline tile_t prevalent_wind() const {
            return suit_type::z + 1u + ((t & prevalent_wind_filter) >> 2);
        }

        inline uint8_t upper_bits() const {
            return t >> 8;
        }

        inline operator win_t() const {
            return t;
        }

        win_type(win_t t) : t(t) {}
        win_type(bool self_d, bool final_t, bool kong_r, bool heavenly_or_earthly_h, tile_t seat_w = 0u, tile_t prevalent_w = 0u, uint32_t upper_bits = 0u) : t(0) {
            if (self_d) t |= self_drawn;
            if (final_t) t |= final_tile;
            if (kong_r) t |= kong_related;
            if (heavenly_or_earthly_h) t |= heavenly_or_earthly_hand;
            t |= (upper_bits & 0b11111111u) << 8;
            if (seat_w == 0u) seat_w = honours::E;
            if (prevalent_w == 0u) prevalent_w = honours::E;
            switch (seat_w) {
                case honours::E: t |= 0b00000000u; break;
                case honours::S: t |= 0b00000001u; break;
                case honours::W: t |= 0b00000010u; break;
                case honours::N: t |= 0b00000011u; break;
            }
            switch (prevalent_w) {
                case honours::E: t |= 0b00000000u; break;
                case honours::S: t |= 0b00000100u; break;
                case honours::W: t |= 0b00001000u; break;
                case honours::N: t |= 0b00001100u; break;
            }
        }
    
    };
    
    class tile {

    private:  
        const tile_t value;

    public:
        constexpr static tile_t invalid = 0u;

        tile(tile_t value) : value(value) {}

        inline int value_in_order() const {
            if (suit() == suit_type::z) {
                return static_cast<int>(value) + 400;
            }
            return static_cast<int>(value);
        }

        inline suit_type suit(bool distinct_dragons = false) const {
            return static_cast<suit_type>(value & (0b11100000u + (distinct_dragons << 4)));
        }

        inline num_t num() const {
            return (value & 0b01000000u) ? (value & 0b00001111u) : 0u;
        }

        operator tile_t() const {
            return value;
        }

        template<typename T> requires std::is_constructible_v<typename T::value_type, tile_t>
        inline bool is_in(const T& tiles) const {
            return std::find(tiles.begin(), tiles.end(), value) != tiles.end();
        }

        template<typename T> requires std::is_constructible_v<T, tile_t>
        inline bool is_in(std::initializer_list<T> tiles) const {
            return std::find(tiles.begin(), tiles.end(), value) != tiles.end();
        }

        friend std::ostream& operator<<(std::ostream& os, const tile& t) {
            if (!(t.value & 0b01000000u)) 
                os << "_ESWNCFP"[t.value & 0b00000111u];
            else os << (int)t.num() << "__mp__s"[t.suit() >> 5];
            return os;
        }

    };

    namespace tile_literals {

        inline tile_t operator""_m(unsigned long long value) {
            return value + suit_type::m;
        }

        inline tile_t operator""_p(unsigned long long value) {
            return value + suit_type::p;
        }

        inline tile_t operator""_s(unsigned long long value) {
            return value + suit_type::s;
        }

        inline tile_t operator""_z(unsigned long long value) {
            using namespace honours;
            return std::basic_string<tile_t>{0u, E, S, W, N, C, F, P}[value];
        }

        inline tile operator""_tile(unsigned long long value) {
            return tile(value);
        }

        inline meld_t operator|(meld_type type, tile_t tile) {
            return static_cast<meld_t>(static_cast<unsigned>(type) | static_cast<unsigned>(tile));
        }

        inline meld_t operator|(tile_t tile, meld_type type) {
            return static_cast<meld_t>(static_cast<unsigned>(type) | static_cast<unsigned>(tile));
        }

    }

    namespace tile_set {

        using namespace tile_literals;
        using namespace honours;
        
        const std::array<tile_t, 34> all_tiles = {
            1_m, 2_m, 3_m, 4_m, 5_m, 6_m, 7_m, 8_m, 9_m,
            1_p, 2_p, 3_p, 4_p, 5_p, 6_p, 7_p, 8_p, 9_p,
            1_s, 2_s, 3_s, 4_s, 5_s, 6_s, 7_s, 8_s, 9_s,
            E, S, W, N, C, F, P
        };
        
        const std::array<tile_t, 27> numbered_tiles = {
            1_m, 2_m, 3_m, 4_m, 5_m, 6_m, 7_m, 8_m, 9_m,
            1_p, 2_p, 3_p, 4_p, 5_p, 6_p, 7_p, 8_p, 9_p,
            1_s, 2_s, 3_s, 4_s, 5_s, 6_s, 7_s, 8_s, 9_s
        };

        const std::array<tile_t, 9> character_tiles = {
            1_m, 2_m, 3_m, 4_m, 5_m, 6_m, 7_m, 8_m, 9_m
        };

        const std::array<tile_t, 9> dot_tiles = {
            1_p, 2_p, 3_p, 4_p, 5_p, 6_p, 7_p, 8_p, 9_p
        };

        const std::array<tile_t, 9> bamboo_tiles = {
            1_s, 2_s, 3_s, 4_s, 5_s, 6_s, 7_s, 8_s, 9_s
        };

        const std::array<tile_t, 13> terminal_honour_tiles = {
            1_m, 9_m, 1_p, 9_p, 1_s, 9_s, E, S, W, N, C, F, P
        };

        const std::array<tile_t, 7> honour_tiles = {
            E, S, W, N, C, F, P
        };

        const std::array<tile_t, 4> wind_tiles = {
            E, S, W, N
        };

        const std::array<tile_t, 3> dragon_tiles = {
            C, F, P
        };

        const std::array<tile_t, 6> terminal_tiles = {
            1_m, 9_m, 1_p, 9_p, 1_s, 9_s
        };

        const std::array<tile_t, 21> simple_tiles = {
            2_m, 3_m, 4_m, 5_m, 6_m, 7_m, 8_m, 
            2_p, 3_p, 4_p, 5_p, 6_p, 7_p, 8_p, 
            2_s, 3_s, 4_s, 5_s, 6_s, 7_s, 8_s
        };

        inline std::array<tile_t, 9> tiles_of_suit(suit_type suit) {
            switch (suit) {
                case suit_type::m:
                    return character_tiles;
                case suit_type::p:
                    return dot_tiles;
                case suit_type::s:
                    return bamboo_tiles;
                default:
                    return {};
            }
        }

        inline std::array<tile_t, 3> tiles_of_number(num_t num) {
            return {tile_t(num | suit_type::m), tile_t(num | suit_type::p), tile_t(num | suit_type::s)};
        }

    }

// meld: (nothing)0000 (fixed)0 (concealed)0 (type)00 (tile)00000000

    class meld {

    private:
        meld_t m;

    public:
        constexpr static tile_t invalid = 0u;

        meld(meld_t m) : m(m) {}
        meld(tile_t tile, meld_type type, bool concealed = true, bool fixed = true) : m(tile | type | (concealed << 10) | (fixed << 11)) {}

        inline ::mahjong::tile tile() const {
            return m & 0b0000000011111111u;
        }

        inline meld_type type() const {
            return static_cast<meld_type>(m & 0b0000001100000000u);
        }

        inline bool concealed() const {
            return m & 0b0000010000000000u;
        }

        inline bool fixed() const {
            return m & 0b0000100000000000u;
        }

        inline operator meld_t() const {
            return m;
        }

        template<typename T> requires std::is_constructible_v<typename std::decay_t<T>::value_type, tile_t>
        inline bool contains(T&& tiles) const {
            for (tile_t ti : tiles)
                if (tile() == ti || (type() == meld_type::sequence && (tile() + 1 == ti || tile() - 1 == ti))) return true;
            return false;
        }

        template<typename T> requires std::is_constructible_v<T, tile_t>
        inline bool contains(std::initializer_list<T> tiles) const {
            for (tile_t ti : tiles)
                if (tile() == ti || (type() == meld_type::sequence && (tile() + 1 == ti || tile() - 1 == ti))) return true;
            return false;
        }

        friend std::ostream& operator<<(std::ostream& os, const meld& m) {
            switch (m.type()) {
                case meld_type::sequence:
                    os << (int)m.tile().num() - 1 << (int)m.tile().num() << mahjong::tile(m.tile() + 1);
                    break;
                case meld_type::triplet:
                    if (m.tile().suit() == suit_type::z) os << m.tile() << m.tile() << m.tile();
                    else os << (int)m.tile().num() << (int)m.tile().num() << m.tile();
                    break;
                case meld_type::kong:
                    if (m.tile().suit() == suit_type::z) os << m.tile() << m.tile() << m.tile() << m.tile();
                    else os << (int)m.tile().num() << (int)m.tile().num() << (int)m.tile().num() << m.tile();
                    break;
            }
            return os;
        }

        operator std::string() const {
            std::stringstream ss;
            ss << *this;
            return ss.str();
        }

    };

    class tile_counter {

    private:
        unsigned long long m_p_count = 0u; // 3 bits per tile. m bits 3--29, p bits 35--61.
        unsigned long long s_z_count = 0u; // 3 bits per tile. s bits 3--29, z bits 35--55.

    public:
        inline void add(tile_t ti, int64_t count = 1) {
            if (ti & 0b10000000u) {
                s_z_count += count << ((ti & 0b00100000u) + 3 * (ti & 0b00001111u));
            } else {
                m_p_count += count << ((ti & 0b00100000u) + 3 * (ti & 0b00001111u));
            }
        }

        inline void add(const meld& m, int64_t count = 1) {
            switch (m.type()) {
                case meld_type::sequence:
                    add(m.tile(), count);
                    add(m.tile() + 1, count);
                    add(m.tile() - 1, count);
                    break;
                case meld_type::triplet:
                    add(m.tile(), count * 3);
                    break;
                case meld_type::kong:
                    add(m.tile(), count * 4);
                    break;
            }
        }

        inline uint8_t count(tile_t ti = tile::invalid) const {
            if (ti == tile::invalid) {
                uint8_t count = 0;
                for (tile_t ti : tile_set::all_tiles)
                    count += tile_counter::count(ti);
                return count;
            }
            if (ti & 0b10000000u)
                return (s_z_count >> ((ti & 0b00100000u) + 3 * (ti & 0b00001111u))) & 0b111u;
            return (m_p_count >> ((ti & 0b00100000u) + 3 * (ti & 0b00001111u))) & 0b111u;
        }

        template<typename T> requires std::is_constructible_v<tile, typename std::decay_t<T>::value_type>
        inline uint8_t count(T&& tiles) const {
            uint8_t cnt = 0;
            for (auto ti : tiles)
                cnt += count(ti);
            return cnt;
        }

        template<typename T> requires std::is_constructible_v<tile, T>
        inline uint8_t count(std::initializer_list<T> tiles) const {
            uint8_t cnt = 0;
            for (auto ti : tiles)
                cnt += count(ti);
            return cnt;
        }

        inline std::vector<tile_t> tiles(bool duplicate = true) const {
            std::vector<tile_t> tiles;
            if (duplicate) {
                for (tile_t ti : tile_set::all_tiles)
                    for (uint8_t i = 0; i < count(ti); ++i)
                        tiles.push_back(ti);
                return tiles;
            }
            for (tile_t ti : tile_set::all_tiles)
                if (count(ti))
                    tiles.push_back(ti);
            return tiles;
        }

        template<typename T> requires std::is_constructible_v<tile, typename std::decay_t<T>::value_type>
        tile_counter(T&& tiles) {
            for (auto ti : tiles)
                add(ti);
        }

        template<typename T> requires std::is_constructible_v<tile, T>
        tile_counter(std::initializer_list<T> tiles) {
            for (auto ti : tiles)
                add(ti);
        }

        template<typename T, typename V> requires (std::is_constructible_v<tile, typename std::decay_t<T>::value_type> && std::is_constructible_v<meld, typename std::decay_t<V>::value_type>)
        tile_counter(T&& tiles, V&& open_melds) {
            for (auto ti : tiles)
                add(ti);
            for (const auto& m : open_melds) {
                meld_type type = m.type();
                switch (type) {
                    case meld_type::sequence:
                        add(m.tile());
                        add(m.tile() + 1);
                        add(m.tile() - 1);
                        break;
                    case meld_type::triplet:
                        add(m.tile(), 3);
                        break;
                    case meld_type::kong:
                        add(m.tile(), 4);
                        break;
                }
            }
        }

        template<typename T, typename V> requires (std::is_constructible_v<tile, T> && std::is_constructible_v<meld, V>)
        tile_counter(std::initializer_list<T> tiles, std::initializer_list<V> open_melds) {
            for (auto ti : tiles)
                add(ti);
            for (const auto& m : open_melds) {
                meld_type type = m.type();
                switch (type) {
                    case meld_type::sequence:
                        add(m.tile());
                        add(m.tile() + 1);
                        add(m.tile() - 1);
                        break;
                    case meld_type::triplet:
                        add(m.tile(), 3);
                        break;
                    case meld_type::kong:
                        add(m.tile(), 4);
                        break;
                }
            }
        }

        template<typename T, typename V> requires (std::is_constructible_v<tile, typename std::decay_t<T>::value_type> && std::is_constructible_v<meld, V>)
        tile_counter(T&& tiles, std::initializer_list<V> open_melds) {
            for (auto ti : tiles)
                add(ti);
            for (const auto& m : open_melds) {
                meld_type type = m.type();
                switch (type) {
                    case meld_type::sequence:
                        add(m.tile());
                        add(m.tile() + 1);
                        add(m.tile() - 1);
                        break;
                    case meld_type::triplet:
                        add(m.tile(), 3);
                        break;
                    case meld_type::kong:
                        add(m.tile(), 4);
                        break;
                }
            }
        }

        template<typename T, typename V> requires (std::is_constructible_v<tile, T> && std::is_constructible_v<meld, typename std::decay_t<V>::value_type>)
        tile_counter(std::initializer_list<T> tiles, V&& open_melds) {
            for (auto ti : tiles)
                add(ti);
            for (const auto& m : open_melds) {
                meld_type type = m.type();
                switch (type) {
                    case meld_type::sequence:
                        add(m.tile());
                        add(m.tile() + 1);
                        add(m.tile() - 1);
                        break;
                    case meld_type::triplet:
                        add(m.tile(), 3);
                        break;
                    case meld_type::kong:
                        add(m.tile(), 4);
                        break;
                }
            }
        }

        inline operator bool() const {
            return m_p_count || s_z_count;
        }

        inline bool operator==(const tile_counter& c) const {
            return m_p_count == c.m_p_count && s_z_count == c.s_z_count;
        }

        inline bool operator==(const std::pair<unsigned long long, unsigned long long>& p) const {
            return m_p_count == p.first && s_z_count == p.second;
        }

        inline unsigned long long operator[](bool m_p_or_s_z) const {
            return m_p_or_s_z ? s_z_count : m_p_count;
        }

        inline tile_counter operator+(const tile_counter& c) const {
            return {m_p_count + c.m_p_count, s_z_count + c.s_z_count};
        }

        inline tile_counter operator+(const meld& m) const {
            tile_counter c(*this);
            c.add(m);
            return c;
        }

        inline tile_counter operator+(tile_t ti) const {
            tile_counter c(*this);
            c.add(ti);
            return c;
        }

        inline tile_counter operator-(const tile_counter& c) const {
            return std::make_pair(m_p_count - c.m_p_count, s_z_count - c.s_z_count);
        }

        inline tile_counter operator-(const meld& m) const {
            tile_counter c(*this);
            c.add(m, -1);
            return c;
        }

        inline tile_counter operator-(tile_t ti) const {
            tile_counter c(*this);
            c.add(ti, -1);
            return c;
        }

        inline tile_counter& operator+=(const tile_counter& c) {
            m_p_count += c.m_p_count;
            s_z_count += c.s_z_count;
            return *this;
        }

        inline tile_counter& operator+=(const meld& m) {
            add(m);
            return *this;
        }

        inline tile_counter& operator+=(tile_t ti) {
            add(ti);
            return *this;
        }

        inline tile_counter& operator-=(const tile_counter& c) {
            m_p_count -= c.m_p_count;
            s_z_count -= c.s_z_count;
            return *this;
        }

        inline tile_counter& operator-=(const meld& m) {
            add(m, -1);
            return *this;
        }

        inline tile_counter& operator-=(tile_t ti) {
            add(ti, -1);
            return *this;
        }

        inline tile_counter& operator=(const tile_counter& c) {
            m_p_count = c.m_p_count;
            s_z_count = c.s_z_count;
            return *this;
        }

        inline bool operator<=(const tile_counter& c) const {
            for (tile_t ti : tile_set::all_tiles)
                if (count(ti) > c.count(ti)) return false;
            return true;
        }

        inline bool operator>=(const tile_counter& c) const {
            for (tile_t ti : tile_set::all_tiles)
                if (count(ti) < c.count(ti)) return false;
            return true;
        }

        tile_counter(const std::pair<unsigned long long, unsigned long long>& p) : m_p_count(p.first), s_z_count(p.second) {}
        tile_counter(unsigned long long m_p_count, unsigned long long s_z_count) : m_p_count(m_p_count), s_z_count(s_z_count) {}

        tile_counter(const tile_counter& c) : m_p_count(c.m_p_count), s_z_count(c.s_z_count) {}
        tile_counter(tile_counter&& c) : m_p_count(c.m_p_count), s_z_count(c.s_z_count) {}
        explicit tile_counter() {}

        friend std::ostream& operator<<(std::ostream& os, const tile_counter& c) {
            bool f = false;
            for (tile_t ti : tile_set::all_tiles) {
                for (uint8_t i = 0; i < c.count(ti); ++i)
                    if (mahjong::tile(ti).suit() == suit_type::z) {
                        os << mahjong::tile(ti);
                    } else {
                        os << (int)mahjong::tile(ti).num();
                        f = true;
                    }
                if (mahjong::tile(ti).num() == 9 && f) {
                    os << "__mp__s"[mahjong::tile(ti).suit() >> 5];
                    f = false;
                }
            }
            return os;
        }

    };

    namespace patterns {

#ifdef MAHJONG_KNITTED_STRAIGHT
        using namespace tile_literals;
        const std::array<tile_counter, 6> knitted_straight_counters = {
            tile_counter({1_m, 4_m, 7_m, 2_p, 5_p, 8_p, 3_s, 6_s, 9_s}),
            tile_counter({1_m, 4_m, 7_m, 2_s, 5_s, 8_s, 3_p, 6_p, 9_p}),
            tile_counter({1_p, 4_p, 7_p, 2_m, 5_m, 8_m, 3_s, 6_s, 9_s}),
            tile_counter({1_p, 4_p, 7_p, 2_s, 5_s, 8_s, 3_m, 6_m, 9_m}),
            tile_counter({1_s, 4_s, 7_s, 2_m, 5_m, 8_m, 3_p, 6_p, 9_p}),
            tile_counter({1_s, 4_s, 7_s, 2_p, 5_p, 8_p, 3_m, 6_m, 9_m})
        };
#endif

    }

#define KNITTED_STRAIGHT_DEFAULT false
#ifdef MAHJONG_KNITTED_STRAIGHT
#undef KNITTED_STRAIGHT_DEFAULT
#define KNITTED_STRAIGHT_DEFAULT true
#endif

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
        typedef std::bitset<320> hand_t;
        
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
        hand(const std::vector<F>& tiles, const std::vector<meld>& melds, F wtile, win_t wtype = 0u, bool winning_tile_included = false, bool knitted_straight = KNITTED_STRAIGHT_DEFAULT) : 
            win_type(wtype), win_tile(wtile), open_melds(melds), closed_counter(tiles), total_counter(tiles, melds), knitted_straight_allowed(knitted_straight) {
            if (!winning_tile_included) {
                closed_counter.add(win_tile);
                total_counter.add(win_tile);
            }
        }

        template<typename F> requires std::is_constructible_v<tile, F>
        hand(std::vector<F>&& tiles, std::vector<meld>&& melds, F wtile, win_t wtype = 0u, bool winning_tile_included = false, bool knitted_straight = KNITTED_STRAIGHT_DEFAULT) : 
            win_type(wtype), win_tile(wtile), open_melds(melds), closed_counter(tiles), total_counter(tiles, melds), knitted_straight_allowed(knitted_straight) {
            if (!winning_tile_included) {
                closed_counter.add(win_tile);
                total_counter.add(win_tile);
            }
        }

        template<typename F> requires std::is_constructible_v<tile, F>
        hand(const std::vector<meld>& all_melds, F pair, F wtile, win_t wtype = 0u, bool knitted_straight = KNITTED_STRAIGHT_DEFAULT) : 
            win_type(wtype), win_tile(wtile), open_melds(), closed_counter({pair, pair}), total_counter({pair, pair}, all_melds), knitted_straight_allowed(knitted_straight) {
            for (const meld& m : all_melds) {
                if (m.fixed()) open_melds.push_back(m);
                else closed_counter.add(m);
            }
        }

        template<typename F> requires std::is_constructible_v<tile, F>
        hand(std::vector<meld>&& all_melds, F pair, F wtile, win_t wtype = 0u, bool knitted_straight = KNITTED_STRAIGHT_DEFAULT) : 
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

    template<typename T, typename tag_type = uint32_t> requires std::is_arithmetic_v<T>
    class scoring_element {

    private:
        std::vector<T> (*p)(const hand&) = nullptr;
        T (*q)(const hand&) = nullptr;

    public:
        const std::string name;
        const tag_type tag;

        scoring_element(std::string name, const tag_type& tag, std::vector<T> (*f)(const hand&)) : p(f), name(name), tag(tag) {} 
        scoring_element(std::string name, std::vector<T> (*f)(const hand&)) : p(f), name(name), tag() {}  

        scoring_element(std::string name, const tag_type& tag, T (*f)(const hand&)) : q(f), name(name), tag(tag) {} 
        scoring_element(std::string name, T (*f)(const hand&)) : q(f), name(name), tag() {}

        inline std::vector<T> operator()(const hand& h) const {
            if (p) return p(h);
            if (q) return std::vector<T>{q(h)};
            return {};
        }

    };

    using verifier = std::function<bool(const hand&)>;

    template<typename R, typename T, typename element_tag_type = uint32_t, typename tag_type = uint32_t, typename detail_type = std::vector<uint8_t>> requires std::is_arithmetic_v<R> && std::is_arithmetic_v<T> && (!std::is_same_v<R, bool>)
    class scoring_system {
        
    private:
        const std::vector<scoring_element<T, element_tag_type>> elements;
        const verifier v;
    
    public:
        class result {

        private:
            bool valid;
            R score;
            tag_type t;
            detail_type d;

        public:
            result(bool valid, R score) : valid(valid), score(score) {}
            result(bool valid, R score, const tag_type& tag) : valid(valid), score(score), t(tag) {}
            result(bool valid, R score, const tag_type& tag, const detail_type& detail) : valid(valid), score(score), t(tag), d(detail) {}

            operator bool() const {
                return valid;
            }

            operator R() const {
                return valid ? score : R(0);
            }

            const tag_type& tag() const {
                return t;
            }

            const detail_type& detail() const {
                return d;
            }

        };
    
    protected:
        const std::function<result(const std::vector<std::vector<T>>&)> score_from_elements;

    public:        
        scoring_system(const std::vector<scoring_element<T, element_tag_type>>& elements, const std::function<result(const std::vector<std::vector<T>>&)>& score_from_elements, const std::function<bool(const hand&)>& ver) : elements(elements), score_from_elements(score_from_elements), v(ver) {}

        inline result operator()(const hand& h) const {
            if (!v(h)) return {false, 0};
            std::vector<std::vector<T>> results;
            for (const auto& e : elements) {
                results.push_back(e(h));
            }
            return score_from_elements(results);
        }

    };

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
            return meld::invalid;
        }

        inline hand parse_hand(const std::string& str, tile_t winning_tile = tile::invalid, win_t win_type = 0u, bool winning_tile_included = false, std::function<bool(char, win_t&)> win_type_parser = nullptr, bool knitted_straight = KNITTED_STRAIGHT_DEFAULT) {
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

#undef KNITTED_STRAIGHT_DEFAULT