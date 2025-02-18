#pragma once
#include "HazardPointers.hpp"   //includes <atomic and cassert>
#include <stdexcept>
#include "Segment.hpp"

#ifndef CACHE_LINE
#define CACHE_LINE 64
#endif

template<class T, class Segment>
class LinkedAdapter{
private:
    static constexpr size_t MAX_THREADS = HazardPointers<Segment*>::MAX_THREADS;
    static constexpr int kHpTail = 0;   //index to access the tail pointer in the HP matrix
    static constexpr int kHpHead = 1;   //index to access the head pointer in the HP matrix
    size_t sizeRing;
    const size_t maxThreads;

    alignas(CACHE_LINE) std::atomic<Segment*> head;
    alignas(CACHE_LINE) std::atomic<Segment*> tail;
    
    //Hazard pointer matrix to ensure memory management on concurrent shared enviroment
    HazardPointers<Segment> HP;

public:
    /**
     * @brief Constructor for the Linked Ring Queue
     * @param SegmentLength     (size_t) sizeRing of the segments
     * @param threads           (size_t) number of threads [default: MAX_THREADS]
     * 
     * @note The constructor initializes the head and tail pointers to a new segment
     */
    LinkedAdapter(size_t SegmentLength, size_t threads = MAX_THREADS):
    sizeRing{SegmentLength},
    maxThreads{threads},
    HP(2,maxThreads)  
    {
#ifndef DISABLE_HAZARD
    assert(maxThreads <= MAX_THREADS);
#endif
        Segment* sentinel = new Segment(sizeRing,maxThreads,0);
        head.store(sentinel, std::memory_order_relaxed);
        tail.store(sentinel, std::memory_order_relaxed);
    }

    /**
     * @brief Destructor for the Linked Ring Queue
     * 
     * Deletes all segments in the queue (also drains the queue)
     */
    ~LinkedAdapter() {
        while(pop(0) != nullptr);
        delete head.load();
    }

    static std::string className(bool padding = true){
        return "Linked" + Segment::className(padding);
    }

    /**
     * @brief Push operation on the linked queue
     * @param item (T*) item to push
     * @param tid (int) thread id
     * 
     * @note this operation is non-blocking and always succeeds
     * @note if not explicitly set, then uses HazardPointers to ensure no memory leaks due to concurrent allocations and deletions
     */
    __attribute__((used,always_inline)) void push(T* item, int tid) {
        Segment* ltail = HP.protect(kHpTail, tail.load(), tid);
        while (true) {
#ifndef DISABLE_HP
            //check for tail inconsistency
            Segment* ltail2 = tail.load();
            if (ltail2 != ltail) {
                ltail = HP.protect(kHpTail, ltail2, tid);
                continue;
            }
#endif
            Segment *lnext = ltail->next.load();
            
            //advance the global head if present
            if (lnext != nullptr) {
                if(tail.compare_exchange_strong(ltail, lnext)){
                    ltail = HP.protect(kHpTail, lnext, tid);
                }
                else {
                    ltail = HP.protect(kHpTail, tail.load(), tid);
                }
                continue;
            }

            //try to push on the current segment
            if (ltail->push(item, tid)) {
                HP.clear(kHpTail, tid);
                return;
            }

            //if push failed then the segment has been closed so try to create a new segment
            Segment* newTail = new Segment(sizeRing,maxThreads,ltail->getNextSegmentStartIndex());
            newTail->push(item, tid);

            Segment* nullNode = nullptr;

            //try to link the new segment
            if (ltail->next.compare_exchange_strong(nullNode, newTail)) {
                tail.compare_exchange_strong(ltail, newTail);   //try to globally set the new tail [not guaranteed]
                HP.clear(kHpTail, tid);
                return;
            } else { 
                delete newTail;
            }

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
        T* item = nullptr;
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
            item = lhead->pop(tid);
            if (item == nullptr){
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
                }
            }
            
            HP.clear(kHpHead, tid);
            return item;       
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
     * @debug the function could be improved by keeping a counter for each thread and making the thread update it with push and pop operations
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

};