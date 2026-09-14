#pragma once

// Reimplementation of std::shuffle that pins the permutation algorithm of
// the libstdc++ implementation (GCC 11).
//
// Wall state is rebuilt from recorded seeds during replay, so the resulting
// order must be stable no matter which standard library implementation hosts
// the backend.  The standard does not specify how std::shuffle consumes the
// generator, therefore the libstdc++ algorithm is reproduced here: elements
// are swapped in pairs using one composed uniform range while the generator
// range is large enough, and the classic Fisher-Yates loop is used
// otherwise.
//
// Keep this file in sync with the recorded behaviour: changing it changes
// how old records are rebuilt from their wall seeds.

#include <iterator>
#include <type_traits>
#include <utility>

#include <algorithm>

#include "random/uniform_int_distribution.h"

namespace mmcr::random {

namespace detail {

template <typename IntType, typename Generator>
auto two_uniform_ints(IntType bound0, IntType bound1, Generator& generator)
        -> std::pair<IntType, IntType> {
    const IntType value = uniform_int_distribution<IntType>{
        0, static_cast<IntType>(bound0 * bound1 - 1)}(generator);
    return {value / bound1, value % bound1};
}

}  // namespace detail

template <typename RandomAccessIterator, typename Generator>
void shuffle(RandomAccessIterator first, RandomAccessIterator last, Generator&& generator) {
    if (first == last) {
        return;
    }

    using DistanceType = typename std::iterator_traits<RandomAccessIterator>::difference_type;
    using UnsignedDistance = std::make_unsigned_t<DistanceType>;
    using Distribution = uniform_int_distribution<UnsignedDistance>;
    using ParamType = typename Distribution::param_type;
    using Engine = std::remove_reference_t<Generator>;
    using Common = std::common_type_t<typename Engine::result_type, UnsignedDistance>;

    const Common urng_range = static_cast<Common>(generator.max() - generator.min());
    const Common range = static_cast<Common>(last - first);

    if (urng_range / range >= range) {
        RandomAccessIterator current = first + 1;

        // An even number of elements means an odd number of swaps, so the
        // first pair is not aligned and is handled separately.
        if ((range % 2) == 0) {
            Distribution distribution{0, 1};
            std::iter_swap(current++, first + distribution(generator));
        }

        while (current != last) {
            const Common swap_range = static_cast<Common>(current - first) + 1;
            const auto positions = detail::two_uniform_ints(swap_range, swap_range + 1, generator);
            std::iter_swap(current++, first + positions.first);
            std::iter_swap(current++, first + positions.second);
        }
        return;
    }

    Distribution distribution;
    for (RandomAccessIterator current = first + 1; current != last; ++current) {
        std::iter_swap(current, first + distribution(generator, ParamType(0, current - first)));
    }
}

}  // namespace mmcr::random
