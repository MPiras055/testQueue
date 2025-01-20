#pragma once
#include <barrier>
#include <atomic>

#define NSEC_IN_SEC 1'000'000'000ULL

struct Data{
    int tid;
    size_t val;
    Data() = default;
    Data(int tid,size_t val): tid(tid), val(val){};
};

//struct for shared arguments
struct threadArgs{
    std::barrier<>* producerBarrier;
    std::barrier<>* consumerBarrier;
    std::atomic<bool>* stopFlag;
    size_t numOps;
    size_t min_wait;
    size_t max_wait;
    size_t producers;
    size_t consumers;
};
