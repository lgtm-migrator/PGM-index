// This file is part of PGM-index <https://github.com/gvinciguerra/PGM-index>.
// Copyright (c) 2020 Giorgio Vinciguerra.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <string>
#include <stdexcept>
#include "sdsl.hpp"
#include "pgm_index.hpp"

/** Computes the smallest integral value not less than x / y, where x and y must be positive integers. */
#define CEIL_UINT_DIV(x, y) ((x) / (y) + ((x) % (y) != 0))

/** Computes the number of bits needed to store x, that is, 0 if x is 0, 1 + floor(log2(x)) otherwise. */
#define BIT_WIDTH(x) ((x) == 0 ? 0 : 64 - __builtin_clzll(x))

/**
 * A simple variant of @ref BinarySearchBasedPGMIndex that builds a top-level lookup table to speed up the search on the
 * segments.
 *
 * If properly tuned and used in a system with a fast internal memory, this variant can be faster than @ref PGMIndex in
 * the average case.
 *
 * The @p TopLevelBitSize template argument allows to specify the bit-size of the memory cells in the top-level table.
 * It can be set either to a power of two or to 0. If set to 0, the bit-size of the cells will be determined dynamically
 * so that the table is bit-compressed.
 *
 * @tparam K the type of the indexed elements
 * @tparam Epsilon the maximum error allowed in the last level of the index
 * @tparam TopLevelBitSize the bit-size of the cells in the top-level table, must be either 0 or a power of two
 * @tparam Floating the floating-point type to use for slopes
 */
template<typename K, size_t Epsilon = 64, size_t TopLevelBitSize = 32, typename Floating = double>
class InMemoryPGMIndex {
protected:
    static_assert(Epsilon > 0);
    static_assert(TopLevelBitSize == 0 || (TopLevelBitSize & (TopLevelBitSize - 1)) == 0);

    using Segment = typename PGMIndex<K, Epsilon, 0, double>::Segment;

    size_t n;                                     ///< The number of elements this index was built on.
    K first_key;                                  ///< The smallest element.
    std::vector<Segment> segments;                ///< The segments composing the index.
    sdsl::int_vector<TopLevelBitSize> top_level;  ///< The structure on the segment.
    K step;

    template<typename RandomIt>
    void build_segments(RandomIt first, RandomIt last, size_t epsilon) {
        assert(std::is_sorted(first, last));
        if (n == 0)
            return;

        first_key = *first;
        segments.reserve(n / (epsilon * epsilon));

        auto ignore_last = *std::prev(last) == std::numeric_limits<K>::max(); // max is reserved for padding
        auto last_n = n - ignore_last;
        last -= ignore_last;

        auto back_check = [this, last](size_t n_segments, size_t prev_level_size) {
            if (segments.back().slope == 0) {
                // Here, we need to ensure that keys > *(last-1) are approximated to a position == prev_level_size
                segments.emplace_back(*std::prev(last) + 1, 0, prev_level_size);
                ++n_segments;
            }
            segments.emplace_back(prev_level_size);
            return n_segments;
        };

        // Build first level
        auto in_fun = [this, first](auto i) {
            auto x = first[i];
            if (i > 0 && i + 1u < n && x == first[i - 1] && x != first[i + 1] && x + 1 != first[i + 1])
                return std::pair<K, size_t>(x + 1, i);
            return std::pair<K, size_t>(x, i);
        };
        auto out_fun = [this](auto, auto, auto cs) { segments.emplace_back(cs); };
        back_check(make_segmentation_par(last_n, epsilon, in_fun, out_fun), last_n);
    }

    template<typename RandomIt>
    void build_top_level(RandomIt, RandomIt last, size_t top_level_size) {
        auto log_segments = (size_t) BIT_WIDTH(segments.size());
        if constexpr (TopLevelBitSize == 0)
            top_level = sdsl::int_vector<>(top_level_size, segments.size(), log_segments);
        else {
            if (log_segments > TopLevelBitSize)
                throw std::invalid_argument("The value TopLevelBitSize=" + std::to_string(TopLevelBitSize) +
                    " is too low. Try to set it to " + std::to_string(TopLevelBitSize << 1));
            top_level = sdsl::int_vector<TopLevelBitSize>(top_level_size, segments.size(), TopLevelBitSize);
        }

        step = (size_t) CEIL_UINT_DIV(*std::prev(last), top_level_size);
        for (auto i = 0ull, k = 0ull; i < top_level_size - 1; ++i) {
            while (k < segments.size() && segments[k].key < (i + 1) * step)
                ++k;
            top_level[i] = k;
        }
    }

    /**
     * Returns the segment responsible for a given key, that is, the rightmost segment having key <= the sought key.
     * @param key the value of the element to search for
     * @return an iterator to the segment responsible for the given key
     */
    auto segment_for_key(const K &key) const {
        auto j = std::min<size_t>(key / step, top_level.size() - 1);
        auto first = segments.begin() + (key < step ? 0 : top_level[j - 1]);
        auto last = segments.begin() + top_level[j];
        auto it = std::upper_bound(first, last, key);
        return it == segments.begin() ? it : std::prev(it);
    }

public:

    static constexpr size_t epsilon_value = Epsilon;

    /**
     * Constructs an empty index.
     */
    InMemoryPGMIndex() = default;

    /**
     * Constructs the index on the given sorted data, with the specified top level size.
     * @param data the vector of keys, must be sorted
     * @param top_level_size the number of cells allocated for the top-level table
     */
    explicit InMemoryPGMIndex(const std::vector<K> &data, size_t top_level_size)
        : InMemoryPGMIndex(data.begin(), data.end(), top_level_size) {}

    /**
     * Constructs the index on the sorted data in the range [first, last), with the specified top level size.
     * @param first, last the range containing the sorted elements to be indexed
     * @param top_level_size the number of cells allocated for the top-level table
     */
    template<typename RandomIt>
    InMemoryPGMIndex(RandomIt first, RandomIt last, size_t top_level_size) : n(std::distance(first, last)) {
        build_segments(first, last, Epsilon);
        build_top_level(first, last, top_level_size);
    }

    /**
     * Returns the approximate position of a key.
     * @param key the value of the element to search for
     * @return a struct with the approximate position
     */
    ApproxPos find_approximate_position(const K &key) const {
        auto k = std::max(first_key, key);
        auto it = segment_for_key(k);
        auto pos = std::min<size_t>((*it)(k), std::next(it)->intercept);
        auto lo = SUB_ERR(pos, Epsilon);
        auto hi = ADD_ERR(pos, Epsilon + 1, n);
        return {pos, lo, hi};
    }

    /**
     * Returns the number of segments in the last level of the index.
     * @return the number of segments
     */
    size_t segments_count() const {
        return segments.size();
    }

    /**
     * Returns the number of levels in the index.
     * @return the number of levels in the index
     */
    size_t height() const {
        return 1;
    }

    /**
     * Returns the size of the index in bytes.
     * @return the size of the index in bytes
     */
    size_t size_in_bytes() const {
        return segments.size() * sizeof(Segment) + top_level.size() * top_level.width();
    }
};