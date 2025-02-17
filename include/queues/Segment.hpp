#pragma once
#include <atomic>
#include <cstddef>
#include <thread>
#include <chrono>

#ifndef CACHE_LINE
#define CACHE_LINE 64
#endif

// #ifndef CLUSTER_WAIT
// #define CLUSTER_WAIT (1ull << 16) //more or less 128.000 nanoseconds
// #endif


/**
 * Superclass for queue segments
 */
template <class T,class Segment>
struct QueueSegmentBase {
public:

    alignas(CACHE_LINE) std::atomic<uint64_t> head{0};
    alignas(CACHE_LINE) std::atomic<uint64_t> tail{0};
    alignas(CACHE_LINE) std::atomic<Segment*> next{nullptr};
    alignas(CACHE_LINE) std::atomic<int> cluster{0};    //initializes the cluster to 0

//     /***
//      * @brief waits for the current thread to be on the same cluster as the segment
//      * 
//      * @param threadCluster (int) the cluster where the thread is executing
//      * 
//      * The function consists of several microsleeps for the thread to wait while checking 
//      * if the current segment cluster is the same as its cluster. If `MAX_SPINS` is reached
//      * then the thread tries to change the current cluster [doesn't check fails - guarantees forward progress]
//      * before exiting
//      * 
//      * @warning this function is useful only when the threads are pinned to a specific CPU [or Numa Node]
//      */
//     __attribute__((always_inline,used)) inline void waitOnCluster(int threadCluster){
// #ifndef DISABLE_CLUSTER
//         int c = cluster.load(std::memory_order_acquire);
//         if(c != threadCluster){
//             std::this_thread::sleep_for(std::chrono::nanoseconds{CLUSTER_WAIT});    //sleep for ~ 128 usec
//         }
//         c = cluster.load(std::memory_order_acquire);
//         cluster.compare_exchange_weak(c,threadCluster,std::memory_order_relaxed);
// #endif  
//         return;
//     }


    /**
     * @brief get the index given the tail [ignores the MSB]
     * 
     * @note the MSB is used to signal a closed segment in segment implementation
     */
    inline uint64_t tailIndex(uint64_t t) const {
        return (t & ~(1ull << 63));
    }

    /**
     * @brief check if the current segment is closed
     */
    inline bool isClosed(uint64_t t) const {
        return (t & (1ull << 63)) != 0;
    }

    /**
     * @brief check if rhe segment is closed without accessing the 
     * tail index
     */
    inline bool isClosed() const {
        return isClosed(tail.load());
    }

    /**
     * @brief sets the starting index given a new segment
     * 
     * @note when starting a new segment the head and the tail are the same
     * @note tipically the index is the tail of the previous segment [for segments FAA based]
     */
    inline void setStartIndex(uint64_t i){
        head.store(i,std::memory_order_relaxed);
        tail.store(i,std::memory_order_relaxed); 
    }

    /**
     * @brief fixes head and tail when they are inconsistent
     * 
     * @note some segments [FAA] perform fetch and add operations regardless of the current state of tail and head
     * @note it can happen that head gets bigger than tail [i.e case of unbalanced load of consumers]
     */
    inline void fixState() {
        while (true) {
            uint64_t t = tail.load();
            uint64_t h = head.load();
            if (tail.load() != t) continue; //inconsistent tail
            if (h > t) { // h would be less than t if queue is closed
                uint64_t tmp = t;
                if (tail.compare_exchange_strong(tmp, h))
                    break;
                continue;
            }
            break;
        }
    }

    /**
     * @brief gets the head of the current segment
     * 
     * @warning the head can be an approximation [i.e for segments FAA based]
     */
    size_t length() const {
        long size = tail.load() - head.load();
        return size > 0 ? size : 0;
    }

    /**
     * @brief set the closed bit of a segment
     * 
     * @param tailticket (uint64_t) the tail of the segment [can fail]
     * @param force (bool) force the closing of the segment [always successful]
     * 
     * @warning tipically when a segment closes it cant be reopened
     * @warning this function should only use standard C++ synchronization primitives
     * 
     * @note the function assumes that the tail is tailticket + 1 so the CAS is
     * made tailticket + 1
     * 
     */
    inline bool closeSegment(const uint64_t tailticket,bool force) {
        if(force){
            tail.fetch_or(1ull << 63);  //implements a TAS on the MSB
            return true;    //we don't need to check the value
        }
        else{
            uint64_t tmp = tailticket + 1;
            return tail.compare_exchange_strong(tmp, tmp | 1ull<<63);
        }
    }

    /**
     * @brief set the closed bit of a segment 
     * @note calls bool closeSegment(const uint64_t)
     */
    inline bool closeSegment(bool force) {
        return closeSegment(tail.load()-1,force); //minus one to not accout for the fetch-add operation
    }

    /**
     * @brief checks if the current semgnet is empty is empty
     */
    inline bool isEmpty() const {
        uint64_t h = head.load();
        uint64_t t = tailIndex(tail.load());
        return h >= t;
    }

    /**
     * @brief getter for head index
     */
    inline uint64_t getHeadIndex() {
        return head.load();
    }

    /**
     * @brief getter for the tail index
     */
    inline uint64_t getTailIndex() {
        return tailIndex(tail.load());
    }

    /**
     * @brief get the start index of the next segment
     */
    inline uint64_t getNextSegmentStartIndex() {
        return getTailIndex() - 1;
    }
};