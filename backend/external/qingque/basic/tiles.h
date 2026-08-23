#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace mahjong {

    using tile_t = uint8_t;
    using num_t = uint8_t;
    using hand_type_t = uint8_t;
    using meld_t = uint16_t;
    using win_t = uint16_t;

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

#ifdef MAHJONG_KNITTED_STRAIGHT
    constexpr bool knitted_straight_default = true;
#else
    constexpr bool knitted_straight_default = false;
#endif

}
