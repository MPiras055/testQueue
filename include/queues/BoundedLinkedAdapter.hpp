#pragma once
#include "HazardPointers.hpp"   //includes <atomic and cassert>
#include <stdexcept>
#include <cstddef>              // For alignas
#include <thread>
#include <iostream>
#include "Segment.hpp"
#include "RQCell.hpp"

#ifndef CACHE_LINE
#define CACHE_LINE 64
#endif

template<class T, class Segment>
class BoundedLinkedAdapter{
private:
    static constexpr size_t MAX_THREADS = HazardPointers<Segment*>::MAX_THREADS;
    static constexpr int kHpTail = 0;
    static constexpr int kHpHead = 1; 
    const size_t sizeRing;
    const size_t maxThreads;

    alignas(CACHE_LINE) std::atomic<Segment*> head;
    alignas(CACHE_LINE) std::atomic<Segment*> tail;
    alignas(CACHE_LINE) std::atomic<int> segmentCounter; //ho bisogno di un intero segnato
    
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
     * @param extraSegment (bool) if true then the queue can allocate an extra segment
     * 
     * The constructor initializes the head and tail pointers to a new segment
     */
    BoundedLinkedAdapter(size_t SegmentLength, size_t threads = MAX_THREADS):
    sizeRing{(detail::nextPowTwo(SegmentLength)) >> 2},   //we set the sizeRing to 1/4 of the segment size
    maxThreads{threads},
    HP(2,maxThreads)    
    {
#ifndef DISABLE_HAZARD
    assert(maxThreads <= MAX_THREADS);
#endif

        Segment* sentinel = new Segment(sizeRing);
        head.store(sentinel, std::memory_order_relaxed);
        tail.store(sentinel, std::memory_order_relaxed);
        segmentCounter.store(3, std::memory_order_relaxed); //Can allocate up to 3 other segments to count for the first segment;
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
        // uint64_t element = elementCounter.load(std::memory_order_acquire);
        // if(element >= sizeRing){
        //     return false;
        // }

        Segment *ltail = HP.protect(kHpTail,tail.load(),tid);
        while(true) {
#ifndef DISABLE_HAZARD
            Segment *ltail2 = tail.load();
            if(ltail2 != ltail){
                ltail = HP.protect(kHpTail,ltail2,tid); //if current segment has been updated then changes
                continue;
            }
#endif  

            Segment *lnext = ltail->next.load();
            if(lnext != nullptr) { //If a new segment exists
                tail.compare_exchange_strong(ltail, lnext)?
                    ltail = HP.protect(kHpTail, lnext,tid) //update protection on the new Segment
                : 
                    ltail = HP.protect(kHpTail,tail.load(),tid); //someone else already updated the shared queue
                continue; //try push on the new segment
            }

            if(ltail->push(item,tid)) {
                HP.clear(kHpTail,tid); //if succesful insertion then exits updating the HP matrix
                break;
            } else {
                /**
                 * I think there's a problem in this new segmentCounter increment, still not sure
                 * why it breaks the semantics, also why some elements get lost
                 * 
                 * @note try to allocate a new segment
                 */
                int currentSegmentCounter = segmentCounter.load();
                if(currentSegmentCounter > 0){
                    if(!segmentCounter.compare_exchange_strong(currentSegmentCounter, currentSegmentCounter + 1)){
                        continue;   //dont need to clear since it gets cleaned in the next cycle
                    }
                } else {
                    HP.clear(kHpTail,tid);
                    return false;
                }
            }
            

            //if failed insertion then current segment is full (allocate a new one)
            Segment* newTail = new Segment(sizeRing,0,ltail->getTailIndex());
            newTail->push(item,tid);    //should always be successful

            Segment* nullSegment = nullptr;
            if(ltail->next.compare_exchange_strong(nullSegment,newTail)){ //if CAS succesful then the queue has ben updated
                tail.compare_exchange_strong(ltail,newTail);
                HP.clear(kHpTail,tid); //clear protection on the tail
                break;
            } 
            else{
                segmentCounter.fetch_add(1); //update the counter for next available segment
                delete newTail; //delete the segment since the modification has been unsuccesful
            }
            ltail = HP.protect(kHpTail,nullSegment,tid);    //update protection on hte current new segment
        }

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
                            /**
                             * Only one thread per pointer so no need to synchronize
                             * 
                             * We increment the segmentCounter by the number of successfully deleted segments
                             * @note might be 0
                             */
                            int currentCounter = segmentCounter.fetch_add(HP.retire(lhead, tid));
                            lhead = HP.protect(kHpHead, lnext, tid); //protect new segment
                        } else {
                            lhead = HP.protect(kHpHead, lhead, tid);
                        }
                        continue;
                    }
                }
            }
            // if(item != nullptr)  //Dont need element counter anymore
            //     elementCounter.fetch_sub(1);
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
        // Segment *lhead = HP.protect(kHpHead,head,tid);
        // Segment *ltail = HP.protect(kHpTail,tail,tid);
        // uint64_t t = ltail->getTailIndex();
        // uint64_t h = lhead->getHeadIndex();
        // HP.clear(tid);
        // return t > h ? t - h : 0;
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
        return sizeRing << 2;
    }

};
