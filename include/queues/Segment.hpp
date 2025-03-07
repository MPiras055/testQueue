#pragma once
#include <atomic>
#include <cstddef>

#ifndef CACHE_LINE
#define CACHE_LINE 64ul
#endif

/***
 * @brief Base superclass for all queue segments
 */
template <class T,class Segment>
struct QueueSegmentBase {

    //used for bitwise arithmetic
    static constexpr uint64_t MSB = 1ull << 63;
    static constexpr uint64_t MASK_63 = ~(MSB);

    //private attributes
    alignas(CACHE_LINE) std::atomic<uint64_t> head{0};
    alignas(CACHE_LINE) std::atomic<uint64_t> tail{0};
    alignas(CACHE_LINE) std::atomic<Segment*> next{nullptr};

public:

    /* ==========| STATIC UTILS | ============= | */
    static inline uint64_t getMSB(uint64_t i) { return i & MSB; }
    static inline uint64_t get63LSB(uint64_t i) { return i & MASK_63; }
    static inline uint64_t setMSB(uint64_t i) { return i | MSB; }


    /* ==========| GETTERS |========== | */

    /** 
     * @brief get the 63_least significant bits of the tail index
     * 
     * @note the MSB is used to signal a closed segment in segment implementation
     */
 
    inline uint64_t tailIndex(uint64_t t) const { return (t & (MASK_63)); }

    inline uint64_t tailIndex() const { return tailIndex(tail.load(std::memory_order_acquire));}

    inline uint64_t headIndex() const { return head.load(std::memory_order_acquire);}

    /**
     * @brief check if the current segment is closed (to further insertions)
     * 
     * @note checks if the MSB of the tail is set
     */
    inline bool isClosed(uint64_t t) const { return (t & (MSB)) != 0; }

    inline bool isClosed() const { return isClosed(tail.load(std::memory_order_acquire)); }
    
    /**
     * @brief get the lenfgth of the segment
     * 
     * @warning this value could be an approximation
     */
    inline size_t length() const {
        long size = tailIndex() - headIndex();
        return size > 0 ? size : 0;
    }

    /**
     * @brief returns true if the segment is empty
     * 
     * @warning this method is subject to false negatives
     */
    inline bool isEmpty() const { return headIndex() >= tailIndex();}

  
    /**
     * @brief used in the linked list adapter to get the start index of "next"
     * 
     * @note used to track the length of the queue (linked list version)
     */
    inline uint64_t getNextSegmentStartIndex() const { return tailIndex() - 1;}


    /* ==========| SETTERS |========== | */

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
     * @brief closes the segment to further insertions
     * @par int64 tail: supposed value for tail
     * @par bool force: [default true] uses atomic_or to guarantee success
     * 
     * @note emulates test_and_set on the MSB of the tail
     */
    inline bool closeSegment(uint64_t t, bool force = false){

        //if it's already closed just return true
        if((t & MSB) != 0) return true;

        if(force)
            tail.fetch_or(MSB,std::memory_order_acq_rel);
        else{
            uint64_t tmp = t + 1; //account for FAA
            return tail.compare_exchange_strong(tmp, tail | MSB, std::memory_order_acq_rel);
        }
        return true;
    }

    /**
     * @brief wrapper that closes the segment without the index value
     */
    inline bool closeSegment(bool force = false) {
        return closeSegment(tail.load(std::memory_order_acquire) - 1, force);
    }

    /**
     * @brief fixes head and tail when they are inconsistent [head > tail]
     */
    inline void fixState() {
        while (1) {
            uint64_t t = tail.load(std::memory_order_acquire);
            uint64_t h = head.load(std::memory_order_acquire);

            if (t != tail.load(std::memory_order_acquire)) //inconsistent tail
                continue;

            if (h > t) {    //never true if the segment is closed
                if (!tail.compare_exchange_strong(t, h, std::memory_order_acq_rel))    //revert the tail to the head value 
                    continue;
            }
            
            return;
        }

    }

};