#pragma once
#include "HazardPointers.hpp"   //includes <atomic and cassert>
#include <iostream>
#include <stdexcept>
#include <cstddef>              // For alignas
#include "Segment.hpp"
#include "RQCell.hpp"

#ifndef CACHE_LINE
#define CACHE_LINE 64
#endif

template<class T, class Segment>
class BoundedLinkedAdapter{
private:
    static constexpr size_t MAX_THREADS = HazardPointers<Segment*>::MAX_THREADS;
    static constexpr int kHpTail = 0;   //index to access the tail pointer in the HP matrix
    static constexpr int kHpHead = 1;   //index to access the head pointer in the HP matrix
    static constexpr size_t MAX_SEGMENTS = 4;   //if this is too big then prformace drops due to pointer undirection
    const size_t sizeRing;
    const size_t maxThreads;

    alignas(CACHE_LINE) std::atomic<Segment*> head;
    alignas(CACHE_LINE) std::atomic<Segment*> tail;
    alignas(CACHE_LINE) std::atomic<uint64_t> tailIndex; //the difference tells us if we can keep allocating new segments
    alignas(CACHE_LINE) std::atomic<uint64_t> headIndex;
    
    HazardPointers<Segment> HP; //Hazard Pointer matrix to ensure no memory leaks on concurrent allocations and deletions

    //Deprecated function
    inline T* dequeueAfterNextLinked(Segment* lhead, int tid) {
        return lhead->pop(tid);
    }

public:
    /**
     * @brief Constructor for the Linked Ring Queue
     * @param SegmentLength (size_t) sizeRing of the segments
     * @param threads (size_t) number of threads [default: MAX_THREADS]
     * 
     * The constructor initializes the head and tail pointers to a new segment
     */
    BoundedLinkedAdapter(size_t SegmentLength, size_t threads = MAX_THREADS):
    //we initialize size to be Seggments / 4 so that max allocated segments account for total queeu length
    sizeRing{(detail::isPowTwo(SegmentLength)? SegmentLength : detail::nextPowTwo(SegmentLength))/MAX_SEGMENTS},
    maxThreads{threads},
    HP(2,maxThreads)    
    {
#ifndef DISABLE_HAZARD
    assert(maxThreads <= MAX_THREADS);
#endif
        if(sizeRing > static_cast<size_t>(std::numeric_limits<int>::max()))
            throw std::invalid_argument("BoundedLinkedAdapter ERROR: sizeRing exceeds the maximum value for an integer");
        Segment* sentinel = new Segment(sizeRing);
        head.store(sentinel, std::memory_order_relaxed);
        tail.store(sentinel, std::memory_order_relaxed);
        headIndex.store(0, std::memory_order_relaxed);
        tailIndex.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Destructor for the Linked Ring Queue
     * 
     * Deletes all segments in the queue (also drains the queue)
     */
    ~BoundedLinkedAdapter() {
        while(pop(0) != nullptr);
        delete head.load();
    }

    static std::string className(bool padding = true){
        return "Bounded" + Segment::className(padding);
    }

    /**
     * @brief Push operation on the linked queue
     * @param item (T*) item to push
     * @param tid (int) thread id
     * 
     * @note this operation is non-blocking and always succeeds
     * @note if not explicitly set, then uses HazardPointers to ensure no memory leaks due to concurrent allocations and deletions
     */
    __attribute__((used,always_inline)) bool push(T* item, int tid){
        if(item == nullptr)
            throw std::invalid_argument(className(false) + "ERROR push(): item cannot be null");
        
        Segment *ltail = HP.protect(kHpTail,tail.load(),tid);   //protect the current segment to prevent deallocation
        while(true) {
#ifndef DISABLE_HAZARD
            Segment *ltail2 = tail.load();  //check if the tail has been updated
            if(ltail2 != ltail){
                ltail = HP.protect(kHpTail,ltail2,tid); //if current segment has been updated then changes
                continue;
            }
#endif  
            Segment *lnext = ltail->next.load();
            if(lnext != nullptr) { //If a new segment exists
                tail.compare_exchange_strong(ltail, lnext)?
                    ltail = HP.protect(kHpTail, lnext,tid) //update protection on the new segment
                : 
                    ltail = HP.protect(kHpTail,tail.load(),tid); //someone else already updated the shared queue so update protection
                continue; //try push on the new segment
            }

            if(ltail->push(item,tid)) { //exit if successful insertion
                HP.clear(kHpTail,tid);
                break;
            } 
            else {
                uint64_t tailIdx = tailIndex.load(std::memory_order_acquire);
                uint64_t headIdx = headIndex.load(std::memory_order_acquire);
                if(tailIdx - headIdx >= MAX_SEGMENTS) { //can't allocate more segments
                    HP.clear(kHpTail,tid);
                    return false;
                } else tailIndex.fetch_add(1,std::memory_order_release); //account for new allocated segemnt
            }

            //if failed insertion then current segment is full (allocate a new one)
            Segment* newTail = new Segment(sizeRing,0,ltail->getTailIndex());
            newTail->push(item,tid);    //push in the segment (always successful since there's no other thread)

            Segment* nullSegment = nullptr;
            if(ltail->next.compare_exchange_strong(nullSegment,newTail)){ //if CAS succesful then the queue has ben updated
                tail.compare_exchange_strong(ltail,newTail);
                HP.clear(kHpTail,tid); //clear protection on the tail before exiting
                break;
            } 
            else {
                delete newTail; //delete the segment since the modification has been unsuccesful
                tailIndex.fetch_sub(1,std::memory_order_release); //account for the failed allocation
            }
            ltail = HP.protect(kHpTail,nullSegment,tid);    //update protection on the current new segment
        }

        //elementCounter.fetch_add(1,std::memory_order_release);
        return true;
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
        Segment* lhead = HP.protect(kHpHead,head.load(),tid);   //protect the current segment
        while(true){
#ifndef DISABLE_HAZARD
            Segment *lhead2 = head.load();
            if(lhead2 != lhead){
                lhead = HP.protect(kHpHead,lhead2,tid);
                continue;
            }           
#endif
            T* item = lhead->pop(tid); //pop on the current segment
            if (item == nullptr) {
                Segment* lnext = lhead->next.load(); //if unsuccesful pop then try to load next semgnet
                if (lnext != nullptr) { //if next segments exist
                    item = lhead->pop(tid); //DequeueAfterNextLinked(lnext)
                    if (item == nullptr) {
                        if (head.compare_exchange_strong(lhead, lnext)) {   //changes shared head pointer
                            uint64_t deleted = HP.retire(lhead, tid); //tries to deallocate current segment
                            lhead = HP.protect(kHpHead, lnext, tid); //protect new segment
                            //update headIndex
                            headIndex.fetch_add(1,std::memory_order_release);  
                        } else {
                            lhead = HP.protect(kHpHead, lhead, tid);
                        }
                        continue;
                    }
                }
            }

            HP.clear(kHpHead,tid); //after pop removes protection on the current segment
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

};