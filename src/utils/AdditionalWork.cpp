#include "AdditionalWork.hpp"
#include <random>
#ifdef DEBUG
#include <cassert>
#endif

static std::random_device random_device;
static thread_local std::minstd_rand random_engine{random_device()};
static thread_local std::uniform_real_distribution<double> random_01_distribution{};

static inline double next_double() {
    return random_01_distribution(random_engine);   //random number between 0 and 1
}

void random_work(const double mean) {
    if (mean < 1.0)
        return;
    const double ref = 1. / mean;   //inverse of mean
    while (next_double() >= ref) {} //check if random number is greater than ref
}

void random_work(size_t inf,size_t sup){
#ifdef DEBUG
    assert(inf <= sup);
#endif
    std::uniform_int_distribution<int> dis(inf, sup); // Distribution between min and max
    loop(dis(random_engine));   //call loop function (shoudn't get optimized);
}