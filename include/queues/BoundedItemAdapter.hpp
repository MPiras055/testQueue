#pragma once
#include "HazardPointers.hpp"   //includes <atomic and cassert>
#include <stdexcept>
#include "Segment.hpp"
#include "RQCell.hpp"

#ifndef CACHE_LINE
#define CACHE_LINE 64
#endif


template<class T, class Segment>
class BoundedItemAdapter{
private:
    static constexpr size_t MAX_THREADS = HazardPointers<Segment*>::MAX_THREADS;
    static constexpr int kHpTail = 0;   //index to access the tail pointer in the HP matrix
    static constexpr int kHpHead = 1;   //index to access the head pointer in the HP matrix
    size_t sizeRing;
    const size_t maxThreads;

    alignas(CACHE_LINE) std::atomic<Segment*> head;
    alignas(CACHE_LINE) std::atomic<Segment*> tail;
    alignas(CACHE_LINE) std::atomic<uint64_t> itemsPushed;
    alignas(CACHE_LINE) std::atomic<uint64_t> itemsPopped;
    
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
    sizeRing{detail::isPowTwo(SegmentLength)? SegmentLength : detail::nextPowTwo(SegmentLength)},
    maxThreads{threads},
    HP(2,maxThreads)    
    {
#ifndef DISABLE_HAZARD
    assert(maxThreads <= MAX_THREADS);
#endif

        assert(sizeRing != 0);
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
            //check if it's safe to push [controllo a fronte]
            if(length() >= sizeRing){
                HP.clear(kHpTail,tid);
                return false;
            }

#ifndef DISABLE_HP
            //check for tail inconsistency
            Segment* ltail2 = tail.load();
            if (ltail2 != ltail) {
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
                continue;
            }
            
            if (ltail->push(item, tid)) {
                itemsPushed.fetch_add(1,std::memory_order_release); //update the item counter
                HP.clear(kHpTail, tid);
                return true;
            }
            

            //if push failed then the segment has been closed so try to create a new segment
            Segment* newTail = new Segment(sizeRing,maxThreads,ltail->getNextSegmentStartIndex());
            newTail->push(item, tid);

            Segment* nullNode = nullptr;

            //try to link the new segment
            if (ltail->next.compare_exchange_strong(nullNode, newTail)) {
                itemsPushed.fetch_add(1,std::memory_order_release); //update the item counter [if successful linking]
                tail.compare_exchange_strong(ltail, newTail);
                HP.clear(kHpTail, tid);
                return true;
            }

            delete newTail;
            ltail = HP.protect(kHpTail, nullNode, tid);
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
            T* item = lhead->pop(tid);
            if (item != nullptr){
                itemsPopped.fetch_add(1,std::memory_order_release);
                HP.clear(kHpHead, tid);
                return item;
                
            }

            Segment* lnext = lhead->next.load();
            if (lnext != nullptr){
                item = lhead->pop(tid);
                if (item == nullptr) {
                    if (head.compare_exchange_strong(lhead, lnext)) {
                        HP.retire(lhead, tid);
                        lhead = HP.protect(kHpHead, lnext, tid);
                    } else{ 
                        lhead = HP.protect(kHpHead, lhead, tid); 
                    }  
                    continue;
                }

                itemsPopped.fetch_add(1,std::memory_order_release);
                HP.clear(kHpHead, tid);
                return item;
            }
            
            //queue apperas empty
            HP.clear(kHpHead, tid);
            return nullptr;       
        }
    }

    /**
     * @brief Returns the current sizeRing of the queue
     * @param tid (int) thread id
     * 
     * @return (size_t) sizeRing of the queue
     * 
     * @note this operation is non-blocking
     * @note the value returned could be an approximation. Depends on the Segment implementation
     * 
     */
    size_t length(int tid) {
        Segment *lhead = HP.protect(kHpHead,head,tid);
        Segment *ltail = HP.protect(kHpTail,tail,tid);
        uint64_t t = ltail->getTailIndex();
        uint64_t h = lhead->getHeadIndex();
        HP.clear(tid);
        return t > h ? t - h : 0;
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

    __attribute__((used,always_inline)) inline size_t length() const {
        return itemsPushed.load(std::memory_order_relaxed) - itemsPopped.load(std::memory_order_relaxed);
    }

};