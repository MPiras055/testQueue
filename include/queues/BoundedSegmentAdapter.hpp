#pragma once
#include "HazardPointers.hpp"
#include <stdexcept>
#include "Segment.hpp"
#include "RQCell.hpp"

#ifndef CACHE_LINE
#define CACHE_LINE 64
#endif

template<class T, class Segment>
class BoundedSegmentAdapter{
private:
    static constexpr size_t MAX_THREADS = HazardPointers<Segment*>::MAX_THREADS;
    static constexpr size_t MAX_SEGMENTS = 4; //maximum number of segments that can be allocated [low or too much pointer chasing]
    static constexpr int kHpTail = 0;   //index to access the tail pointer in the HP matrix
    static constexpr int kHpHead = 1;   //index to access the head pointer in the HP matrix
    const size_t maxSegments;
    const size_t sizeRing;
    const size_t maxThreads;

    alignas(CACHE_LINE) std::atomic<Segment*> head;
    alignas(CACHE_LINE) std::atomic<Segment*> tail;
    alignas(CACHE_LINE) std::atomic<uint64_t> segmentTail; //the difference tells us if we can keep allocating new segments
    alignas(CACHE_LINE) std::atomic<uint64_t> segmentHead;
    
    HazardPointers<Segment> HP; //Hazard Pointer matrix to ensure no memory leaks on concurrent allocations and deletions

public:
    /**
     * @brief Constructor for the BoundedSegmentQueue
     * @param size_par (size_t) sizeRing of the segments
     * @param threads (size_t) number of threads [default: MAX_THREADS]
     * 
     * The constructor initializes the head and tail pointers to a new segment
     */
    BoundedSegmentAdapter(size_t size_par, size_t threads = MAX_THREADS, size_t segmentSize = 4):
    //we initialize size to be Seggments / 4 so that max allocated segments account for total queeu length
    maxSegments{segmentSize},
#ifndef DISABLE_POW2
    sizeRing{(detail::isPowTwo(size_par)? size_par : detail::nextPowTwo(size_par))/segmentSize},
#else 
    sizeRing{size_par},
#endif
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

        segmentHead.store(0, std::memory_order_relaxed);
        segmentTail.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Destructor for the Linked Ring Queue
     * 
     * Deletes all segments in the queue (also drains the queue)
     */
    ~BoundedSegmentAdapter() {
        while(pop(0) != nullptr);
        delete head.load();
    }

    static std::string className(bool padding = true){
        return "BoundedSegment" + Segment::className(padding);
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

            //try to push on the current segment
            if (ltail->push(item, tid)) {
                HP.clear(kHpTail, tid);
                return true;
            }

            uint64_t currentTail = segmentTail.load(std::memory_order_acquire);
            uint64_t currentHead = segmentHead.load(std::memory_order_acquire);

            //can't allocate more segments
            if(currentTail - currentHead >= maxSegments){
                HP.clear(kHpTail, tid);
                return false;
            }
            

            //if push failed then the segment has been closed so try to create a new segment
            Segment* newTail = new Segment(sizeRing,maxThreads,0);
            newTail->push(item, tid);

            Segment* nullNode = nullptr;

            //try to link the new segment
            if (ltail->next.compare_exchange_strong(nullNode, newTail)) {
                Segment* oldTail = ltail;
                tail.compare_exchange_strong(ltail, newTail);
                currentTail = segmentTail.load(std::memory_order_acquire);
                currentHead = segmentHead.load(std::memory_order_acquire);
                /**
                 * The segment shouldn't have been allocated so it gets closed
                 * and doesn't accout as new tail;
                 */
                if(currentTail - currentHead >= maxSegments){
                    //set the new segment as closed
                    oldTail->closeSegment(true); //force closing the segment [always successful]
                }
                segmentTail.fetch_add(1,std::memory_order_release);
                HP.clear(kHpTail, tid);
                return true;
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
            if (item == nullptr){

                Segment* lnext = lhead->next.load();
                if (lnext != nullptr){
                    item = lhead->pop(tid);
                    if (item == nullptr) {
                        if (head.compare_exchange_strong(lhead, lnext)) {
                            HP.retire(lhead, tid);
                            segmentHead.fetch_add(1,std::memory_order_release);
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
     */
    size_t length(int tid) {
        return 0;
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

    uint64_t getSegmentCount() const{
        return segmentTail.load(std::memory_order_acquire);
    }

};