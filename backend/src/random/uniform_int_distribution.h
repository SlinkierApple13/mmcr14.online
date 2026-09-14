#pragma once

// Reimplementation of std::uniform_int_distribution that pins the numeric
// behaviour of the libstdc++ implementation (GCC 11).
//
// Wall state is rebuilt from recorded seeds during replay, so the generated
// numbers must be stable no matter which standard library implementation
// hosts the backend.  The standard does not specify the algorithm used by
// std::uniform_int_distribution, therefore the libstdc++ algorithm is
// reproduced here:
//   * Lemire's nearly divisionless reduction for engines whose range is
//     exactly 32 or 64 bits wide (e.g. mt19937, mt19937_64);
//   * a scaling rejection loop for every other generator.
//
// Keep this file in sync with the recorded behaviour: changing it changes
// how old records are rebuilt from their wall seeds.

#include <cstdint>
#include <limits>
#include <type_traits>

namespace mmcr::random {

namespace detail {

#if defined(__SIZEOF_INT128__)
__extension__ typedef unsigned __int128 WideUint;
#endif

// Lemire's nearly divisionless reduction from a full generator result to
// [0, range).  Wide must be an unsigned type twice as wide as UInt.
template <typename Wide, typename UInt, typename Generator>
auto reduce_nearly_divisionless(Generator& generator, UInt range) -> UInt {
    static_assert(std::is_unsigned_v<UInt>);
    static_assert(sizeof(Wide) == 2 * sizeof(UInt));

    Wide product = static_cast<Wide>(generator()) * static_cast<Wide>(range);
    UInt low = static_cast<UInt>(product);
    if (low < range) {
        const UInt threshold = static_cast<UInt>(UInt{0} - range) % range;
        while (low < threshold) {
            product = static_cast<Wide>(generator()) * static_cast<Wide>(range);
            low = static_cast<UInt>(product);
        }
    }
    return static_cast<UInt>(product >> std::numeric_limits<UInt>::digits);
}

}  // namespace detail

template <typename IntType = int>
class uniform_int_distribution {
    static_assert(std::is_integral_v<IntType>, "template argument must be an integral type");

public:
    using result_type = IntType;

    struct param_type {
        param_type() : param_type(0) {}

        explicit param_type(result_type a,
                            result_type b = std::numeric_limits<result_type>::max())
            : a_(a), b_(b) {}

        [[nodiscard]] auto a() const -> result_type { return a_; }
        [[nodiscard]] auto b() const -> result_type { return b_; }

        friend auto operator==(const param_type&, const param_type&) -> bool = default;

    private:
        result_type a_;
        result_type b_;
    };

    uniform_int_distribution() : uniform_int_distribution(0) {}

    explicit uniform_int_distribution(
            result_type a,
            result_type b = std::numeric_limits<result_type>::max())
        : param_(a, b) {}

    explicit uniform_int_distribution(const param_type& param) : param_(param) {}

    void reset() {}

    [[nodiscard]] auto a() const -> result_type { return param_.a(); }
    [[nodiscard]] auto b() const -> result_type { return param_.b(); }

    [[nodiscard]] auto param() const -> param_type { return param_; }
    void param(const param_type& param) { param_ = param; }

    [[nodiscard]] auto min() const -> result_type { return a(); }
    [[nodiscard]] auto max() const -> result_type { return b(); }

    template <typename Generator>
    auto operator()(Generator& generator) -> result_type {
        return (*this)(generator, param_);
    }

    template <typename Generator>
    auto operator()(Generator& generator, const param_type& param) -> result_type {
        using GeneratorResult = typename Generator::result_type;
        using UnsignedResult = std::make_unsigned_t<result_type>;
        using Common = std::common_type_t<GeneratorResult, UnsignedResult>;

        constexpr Common urng_min = Generator::min();
        constexpr Common urng_max = Generator::max();
        static_assert(urng_min < urng_max, "generator must define min() < max()");
        constexpr Common urng_range = urng_max - urng_min;

        const Common range = static_cast<Common>(param.b()) - static_cast<Common>(param.a());

        Common value = 0;
        if (urng_range > range) {
            // Downscaling: reject a high part of the generator output and
            // reduce the accepted part to the requested range.
            const Common uerange = range + 1;
#if defined(__SIZEOF_INT128__)
            if constexpr (urng_range == std::numeric_limits<std::uint64_t>::max()) {
                // 64-bit engines reduce through a 128-bit product.
                value = static_cast<Common>(detail::reduce_nearly_divisionless<
                            detail::WideUint, std::uint64_t>(
                        generator, static_cast<std::uint64_t>(uerange)));
            } else
#endif
            if constexpr (urng_range == std::numeric_limits<std::uint32_t>::max()) {
                // 32-bit engines reduce through a 64-bit product.
                value = static_cast<Common>(detail::reduce_nearly_divisionless<
                            std::uint64_t, std::uint32_t>(
                        generator, static_cast<std::uint32_t>(uerange)));
            } else {
                const Common scaling = urng_range / uerange;
                const Common past = uerange * scaling;
                do {
                    value = static_cast<Common>(generator()) - urng_min;
                } while (value >= past);
                value /= scaling;
            }
        } else if (urng_range < range) {
            // Upscaling: combine several generator draws into one value.
            Common tmp = 0;
            do {
                const Common step = urng_range + 1;
                tmp = step * (*this)(generator,
                                     param_type(0, static_cast<result_type>(range / step)));
                value = tmp + (static_cast<Common>(generator()) - urng_min);
            } while (value > range || value < tmp);
        } else {
            value = static_cast<Common>(generator()) - urng_min;
        }

        return static_cast<result_type>(value + static_cast<Common>(param.a()));
    }

private:
    param_type param_;
};

}  // namespace mmcr::random
