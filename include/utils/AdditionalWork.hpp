#pragma once
#include <cstdlib>
#include <random>
#ifdef DEBUG
#include <cassert>
#endif

static std::random_device random_device;
static thread_local std::minstd_rand random_engine{random_device()};
static thread_local std::uniform_real_distribution<double> random_01_distribution{};


/**
 * Random number between 0 and 1
 */
static inline double next_double() {
    return random_01_distribution(random_engine);
}

/**
 * @brief loop function that should not be optimized out
 */
inline __attribute__((used,always_inline)) void loop(size_t stop) {
    while(stop-- != 0){
        asm volatile ("nop");
    }
}

static inline void random_work(const double mean) {
    if (mean >= 1.0){
        const double ref = 1. / mean;
        while (next_double() >= ref);
    }
    return;
}

inline __attribute__((used,always_inline)) size_t randint(size_t center, size_t amplitude){
#ifdef DEBUG
    assert(amplitude <= center);
#endif
    double random_amplitude = random_01_distribution(random_engine) * static_cast<double>(amplitude << 1);
    return static_cast<size_t>(static_cast<double>(center-amplitude) + random_amplitude); 
}

/**
 * @brief Random work function
 * 
 * @param center (size_t) center of the distribution
 * @param amplitude (size_t) amplitude of the distribution
 * 
 * loops for a random number between center - amplitude and center + amplitude
 */
__attribute__((used,always_inline)) inline void random_work(size_t center,size_t amplitude){
    loop(randint(center,amplitude));
}

/**
 * @brief Random number between 0 and max
 */
__attribute__((used,always_inline)) inline size_t randint(size_t max){
    return randint(0,max);
}