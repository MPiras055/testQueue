#pragma once

#include <array>
#include <cstddef>
#include <cassert>

#define CACHE_LINE 64

template <size_t cell_size, size_t cache_line_size>
class CacheRemap {
private:
    static_assert(cache_line_size % cell_size == 0);

    static constexpr size_t cellsPerCacheLine = cache_line_size / cell_size;

    const size_t size;
    const size_t numCacheLines;

public:

    CacheRemap(size_t size_par) : size(size_par), numCacheLines((size_par * cell_size) / cache_line_size) {
        assert(cache_line_size > 0);
        assert(numCacheLines > 0);
        assert(size * cell_size % cache_line_size == 0);
    }

    constexpr inline size_t operator [](size_t i) const noexcept __attribute__((always_inline)) {
        return i % numCacheLines * cellsPerCacheLine + i / numCacheLines;
        
    }
};

/**
 * Never used since we assume for lengths that cannot be hold in a single cache line
 */
class IdentityRemap {
public:
    constexpr inline size_t operator [](size_t i) const noexcept __attribute__((always_inline)) {
        return i;
    }
};
