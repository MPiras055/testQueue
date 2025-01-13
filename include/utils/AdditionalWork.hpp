#pragma once
#include <cstdlib>

void random_work(double mean);

void random_work(size_t inf,size_t sup);

/**
 * @brief loop function that should not be optimized out
 */
inline __attribute__((used,always_inline)) void loop(size_t stop) {
    size_t i = 0;
    while (i < stop) {
        ++i;
    }
}