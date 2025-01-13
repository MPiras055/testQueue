#pragma once

#include <atomic>
#include <cstddef>  // For alignas

#ifndef CACHE_LINE
#define CACHE_LINE 64
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

template<class T>
struct PlainCell<T,false>{
    std::atomic<T>          val;
};


}