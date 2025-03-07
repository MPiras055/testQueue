#pragma once

#include <atomic>
#include <cstddef>  // For alignas
#include <cassert>  //for assert

#ifndef CACHE_LINE
#define CACHE_LINE 64ul
#endif

namespace detail{

/**
 * @brief check if a number is a power of 2
 */
inline bool isPowTwo(size_t x){
    return (x != 0 && (x & (x-1)) == 0);
}

/**
 * @brief returns the next power of 2
 */
inline size_t nextPowTwo(size_t x){
    if(isPowTwo(x)) x++;
    size_t p=1;
    while (x>p) p <<= 1;
    return p;
}

template<class, bool padded>
struct CRQCell;

template<class T>
struct alignas(CACHE_LINE) CRQCell<T,true>{
    std::atomic<T>          val;
    std::atomic<uint64_t>   idx;
    char __pad[CACHE_LINE - (sizeof(std::atomic<T>) + sizeof(std::atomic<uint64_t>))];

};

template<class T>
struct alignas(16) CRQCell<T,false>{
    std::atomic<T>          val;
    std::atomic<uint64_t>   idx;
};

template<class T,bool padded>
struct PlainCell;

template<class T>
struct alignas(CACHE_LINE) PlainCell<T,true>{
    std::atomic<T>          val;
    char __pad[CACHE_LINE - sizeof(std::atomic<T>)];
};


/**
 * used for FAA segment, every thread has a length tracker that the thread locally updates
 * 
 * whom tries to compute length has to atomically read these counters, and sum them up
 */
struct alignas(CACHE_LINE) __length_tracker{
    std::atomic<uint64_t> itemsPushed{0};
    std::atomic<uint64_t> itemsPopped{0};
    char __pad[CACHE_LINE - (2*sizeof(std::atomic<uint64_t>))];
};

/**
 * this struct is used to precompute the reserved value (For LPRQ) and stores the numa node
 * 
 * useful when pinning threads to numa nodes
 */
struct alignas(CACHE_LINE) threadInfo{
    int numa_node{0};
    char __pad[CACHE_LINE - (sizeof(int))];
};

template<class T>
struct PlainCell<T,false>{
    std::atomic<T> val;
};

}