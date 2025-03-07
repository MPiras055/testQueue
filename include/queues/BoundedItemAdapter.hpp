#pragma once
#include "HazardPointers.hpp"   //includes <atomic and cassert>
#include <stdexcept>
#include "Segment.hpp"
#include "RQCell.hpp"


#ifndef CACHE_LINE
#define CACHE_LINE 64ul
#endif

template<class T, class Segment>
class BoundedItemAdapter{
private:
    static constexpr size_t MAX_THREADS = HazardPointers<Segment*>::MAX_THREADS;
    static constexpr int kHpTail = 0;   //index to access the tail pointer in the HP matrix
    static constexpr int kHpHead = 1;   //index to access the head pointer in the HP matrix
    const size_t sizeRing;
    const size_t maxThreads;

    alignas(CACHE_LINE) std::atomic<Segment*> head;
    alignas(CACHE_LINE) std::atomic<Segment*> tail;
    alignas(CACHE_LINE) std::atomic<uint64_t> itemsPushed;
    alignas(CACHE_LINE) std::atomic<uint64_t> itemsPopped;
    alignas(CACHE_LINE) static inline thread_local bool check_push = false;
    
    HazardPointers<Segment> HP; //Hazard Pointer matrix to ensure no memory leaks on concurrent allocations and deletions

public:
    /**
     * @brief Constructor for the Linked Ring Queue
     * @param SegmentLength (size_t) sizeRing of the segments
     * @param threads (size_t) number of threads [default: MAX_THREADS]
     * 
     * The constructor initializes the head and tail pointers to a new segment
     */
    BoundedItemAdapter(size_t SegmentLength, size_t threads = MAX_THREADS):
#ifndef DISABLE_POW2
    sizeRing{SegmentLength > 1 && detail::isPowTwo(SegmentLength)? SegmentLength : detail::nextPowTwo(SegmentLength)},
#else
    sizeRing{SegmentLength},
#endif
    maxThreads{threads},
    HP(2,maxThreads)    
    {
#ifdef DEBUG
#ifndef DISABLE_HAZARD
    assert(maxThreads <= MAX_THREADS);
#endif
    assert(sizeRing != 0);
#endif
        Segment* sentinel = new Segment(sizeRing);

        head.store(sentinel, std::memory_order_relaxed);
        tail.store(sentinel, std::memory_order_relaxed);
        itemsPushed.store(0, std::memory_order_relaxed);
        itemsPopped.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Destructor for the Linked Ring Queue
     * 
     * Deletes all segments in the queue (also drains the queue)
     */
    ~BoundedItemAdapter() {
        while(pop(0) != nullptr);
        delete head.load();
    }

    static std::string className(bool padding = true){
        return "BoundedItem" + Segment::className(padding);
    }

    /**
     * @brief Push operation on the linked queue
     * @param item (T*) item to push
     * @param tid (int) thread id
     * 
     * @note this operation is non-blocking and always succeeds
     * @note if not explicitly set, then uses HazardPointers to ensure no memory leaks due to concurrent allocations and deletions
     */
    __attribute__((used,always_inline)) bool push(T* item, int tid) {
        Segment* ltail = HP.protect(kHpTail, tail.load(), tid);
        while (true) {
            //check if it's safe to push
            if(lengthAcquire() >= sizeRing){
                HP.clear(kHpTail,tid);
                return false;
            }

#ifndef DISABLE_HP
            //check for tail inconsistency
            Segment* ltail2 = tail.load();
            if (ltail2 != ltail) {
                check_push = false;
                ltail = HP.protect(kHpTail, ltail2, tid);
                continue;
            }
#endif
            Segment *lnext = ltail->next.load();
            //advance the global head
            if (lnext != nullptr) {
                (tail.compare_exchange_strong(ltail, lnext))?
                    ltail = HP.protect(kHpTail, lnext, tid)
                :
                    ltail = HP.protect(kHpTail, tail.load(), tid);
                check_push = false;
                continue;
            }
            
            if(check_push) check_push = ltail->isClosed();

            if(!check_push){    //if current tail is closed then don't push
                if(ltail->push(item,tid)){
                    itemsPushed.fetch_add(1,std::memory_order_release);
                    HP.clear(kHpTail,tid);
                    return true;
                } else check_push = true;
            }

            //if push failed then the segment has been closed so try to create a new segment
            Segment* newTail = new Segment(sizeRing,maxThreads,ltail->getNextSegmentStartIndex());
            newTail->push(item, tid);

            Segment* nullNode = nullptr;

            //try to link the new segment
            if (ltail->next.compare_exchange_strong(nullNode, newTail)) {
                itemsPushed.fetch_add(1,std::memory_order_release); //update the item counter [if successful linking]
                tail.compare_exchange_strong(ltail, newTail);
                check_push = false;
                HP.clear(kHpTail, tid);
                return true;
            }

            delete newTail;
            ltail = HP.protect(kHpTail, nullNode, tid);
            check_push = false;
        }
    }

    /**
     * @brief Pop operation on the linked queue
     * @param tid (int) thread id
     * 
     * @return (T*) item popped from the queue [nullptr if the queue is empty]
     * 
     * @note this operation is non-blocking
     * @note if not explicitly set, then uses HazardPointers to ensure no memory leaks due to concurrent allocations and deletions
     */
    __attribute__((used,always_inline)) T* pop(int tid) {
        Segment* lhead = HP.protect(kHpHead, head.load(), tid);
        T* item = nullptr;

        while (true) {
#ifndef DISABLE_HP
            //check for tail inconsistency
            Segment* lhead2 = head.load();
            if (lhead2 != lhead) {
                lhead = HP.protect(kHpHead, lhead2, tid);
                continue;
            }
#endif
            //try to pop from the current segment
            if (item = lhead->pop(tid) ; item != nullptr) break;

            Segment* lnext = lhead->next.load();
            if (lnext == nullptr){
                HP.clear(kHpHead, tid);
                return item;    //at this point it's nullptr [failed extraction]
            }
                

            if(item = lhead->pop(tid); item != nullptr) break;
            
            if (head.compare_exchange_strong(lhead, lnext)) {
                HP.retire(lhead, tid);
                lhead = HP.protect(kHpHead, lnext, tid);
            } else{ 
                lhead = HP.protect(kHpHead, lhead, tid); 
            }       
            
        }

        HP.clear(kHpHead, tid);
        itemsPopped.fetch_add(1,std::memory_order_release);
        return item;
    }

    /**
     * @brief returns the size of the queue
     * 
     * @returns (size_t) size of the queue
     * @note it's useful when POW2 is disabled since the size attribute can differ from the parameter
     * @warning if the queue uses segments of different sizes then the value returned could be incorrect
     */
    inline size_t capacity() const {
        return sizeRing;
    }


    /**
     * @brief Returns the current sizeRing of the queue
     * @param tid (int) thread id [keeps interface]
     * 
     * @return (size_t) sizeRing of the queue
     * 
     * @note this operation is non-blocking
     */
    __attribute__((used,always_inline)) inline size_t length([[maybe_unused]] const int tid = 0) const {
        return itemsPushed.load(std::memory_order_relaxed) - itemsPopped.load(std::memory_order_relaxed);
    }

private:
    __attribute__((used,always_inline)) inline size_t lengthAcquire() const {
        return itemsPushed.load(std::memory_order_acquire) - itemsPopped.load(std::memory_order_acquire);
    }

};