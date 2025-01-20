#pragma once
#include <atomic>
/**
 * Superclass for queue segments
 */
template <class T,class Segment>
struct QueueSegmentBase {
public:

    alignas(CACHE_LINE) std::atomic<uint64_t> head{0};
    alignas(CACHE_LINE) std::atomic<uint64_t> tail{0};
    alignas(CACHE_LINE) std::atomic<Segment*> next{nullptr};



    /**
     * @brief get the index given the tail [ignores the MSB]
     * 
     * @note the MSB is used to signal a closed segment in segment implementation
     */
    static inline uint64_t tailIndex(uint64_t tail) {return (tail & ~(1ull << 63));}

    /**
     * @brief check if the current segment is closed
     */
    static inline bool isClosed(uint64_t tail) {return (tail & (1ull<<63)) != 0;}

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
    void fixState() {
        while(true) {
            uint64_t t = tail.load();
            uint64_t h = head.load();
            if(tail.load() != t) continue;
            if(h>t) {
                uint64_t tmp = t;
                if(tail.compare_exchange_strong(tmp,h))
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
     * @warning tipically when a segment closes it cant be reopened
     * @warning this function should only use standard C++ synchronization primitives
     * 
     */
    inline bool closeSegment(const uint64_t tailTicket) {
        // if(!force) {
        //     uint64_t tmp = tailTicket +1;
        //     return tail.compare_exchange_strong(tmp,(tailTicket + 1) | (1ull << 63));
        // } else {
        //     return BIT_TEST_AND_SET63(&(tail));
        // }
        uint64_t tmp = tailTicket + 1;
        return tail.compare_exchange_strong(tmp,(tailTicket + 1) | (1ull << 63));
    }

    /**
     * @brief checks if the current semgnet is empty is empty
     */
    inline bool isEmpty() const {
        return head.load() >= tailIndex(tail.load());
    }

    /**
     * @brief getter for head index
     */
    inline uint64_t getHeadIndex() const { 
        return (head.load()); 
    }

    /**
     * @brief getter for the tail index
     */
    inline uint64_t getTailIndex() const {
        return tailIndex(tail.load());
    }

    /**
     * @brief get the start index of the next segment
     */
    inline uint64_t getNextSegmentStartIndex() const { 
        return getTailIndex() - 1;
    }
};