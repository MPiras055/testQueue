#pragma once
#include "HazardPointers.hpp"   //includes <atomic and cassert>
#include <stdexcept>
#include <cstddef>              // For alignas
#include <thread>
#include "Segment.hpp"

#ifndef CACHE_LINE
#define CACHE_LINE 64
#endif

template<class T, class Segment>
class LinkedRingQueue{
private:
    static constexpr size_t MAX_THREADS = HazardPointers<Segment*>::MAX_THREADS;
    static constexpr int kHpTail = 0;   //index to access the tail pointer in the HP matrix
    static constexpr int kHpHead = 1;   //index to access the head pointer in the HP matrix
    const size_t sizeRing;
    const size_t maxThreads;

    alignas(CACHE_LINE) std::atomic<Segment*> head;
    alignas(CACHE_LINE) std::atomic<Segment*> tail;
    
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
    LinkedRingQueue(size_t SegmentLength, size_t threads = MAX_THREADS):
    sizeRing{SegmentLength},
    maxThreads{threads},
    HP(2,maxThreads)    
    {
#ifndef DISABLE_HAZARD
    assert(maxThreads <= MAX_THREADS);
#endif
        Segment* sentinel = new Segment(SegmentLength);
        head.store(sentinel, std::memory_order_relaxed);
        tail.store(sentinel, std::memory_order_relaxed);
    }

    /**
     * @brief Destructor for the Linked Ring Queue
     * 
     * Deletes all segments in the queue (also drains the queue)
     */
    ~LinkedRingQueue() {
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
    __attribute__((used,always_inline)) void push(T* item, int tid){
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

            //if failed insertion then current segment is full (allocate a new one)
            Segment* newTail = new Segment(sizeRing,0,ltail->getTailIndex()-1);
            newTail->push(item,tid);    //push in the segment (always successful since there's no other thread)

            Segment* nullSegment = nullptr;
            if(ltail->next.compare_exchange_strong(nullSegment,newTail)){ //if CAS succesful then the queue has ben updated
                tail.compare_exchange_strong(ltail,newTail);
                HP.clear(kHpTail,tid); //clear protection on the tail before exiting
                break;
            } 
            else 
                delete newTail; //delete the segment since the modification has been unsuccesful

            ltail = HP.protect(kHpTail,nullSegment,tid);    //update protection on the current new segment
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
                            HP.retire(lhead, tid); //tries to deallocate current segment
                            lhead = HP.protect(kHpHead, lnext, tid); //protect new segment
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