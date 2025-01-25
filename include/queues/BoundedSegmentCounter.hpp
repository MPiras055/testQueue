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
    const size_t MAX_SEGMENTS = 4;   //if this is too big then prformace drops due to pointer undirection
    const size_t sizeRing;
    const size_t maxThreads;

    alignas(CACHE_LINE) std::atomic<Segment*> head;
    alignas(CACHE_LINE) std::atomic<Segment*> tail;
    alignas(CACHE_LINE) std::atomic<uint64_t> segmentTail; //the difference tells us if we can keep allocating new segments
    alignas(CACHE_LINE) std::atomic<uint64_t> segmentHead;
    
    HazardPointers<Segment> HP; //Hazard Pointer matrix to ensure no memory leaks on concurrent allocations and deletions

public:
    /**
     * @brief Constructor for the Linked Ring Queue
     * @param SegmentLength (size_t) sizeRing of the segments
     * @param threads (size_t) number of threads [default: MAX_THREADS]
     * 
     * The constructor initializes the head and tail pointers to a new segment
     */
    BoundedLinkedAdapter(size_t SegmentLength, size_t threads = MAX_THREADS, size_t segmentSize = 4):
    //we initialize size to be Seggments / 4 so that max allocated segments account for total queeu length
    MAX_SEGMENTS{segmentSize},
    sizeRing{(detail::isPowTwo(SegmentLength)? SegmentLength : detail::nextPowTwo(SegmentLength))/segmentSize},
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

        segmentHead.store(0, std::memory_order_relaxed);
        segmentTail.store(0, std::memory_order_relaxed);
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

        //Upon entering protect the the current tail;
        Segment *ltail = HP.protect(kHpTail,tail.load(std::memory_order_acquire),tid);
        while(true) {
#ifndef DISABLE_HAZARD
            //check for local tail consistency
            Segment *ltail2 = tail.load(std::memory_order_acquire);
            if(ltail2 != ltail){
                ltail = HP.protect(kHpTail,ltail2,tid);
                continue;
            }
#endif  
            Segment *lnext = ltail->next.load();
            if(lnext != nullptr) { //if someone pushed a new segment try to update the global tail
                tail.compare_exchange_strong(ltail, lnext)?
                    ltail = HP.protect(kHpTail, lnext,tid)  //if CAS successful then update protection
                : 
                    ltail = HP.protect(kHpTail,tail.load(std::memory_order_acquire),tid); //if CAS failed read new tail and update protection    
                continue; //try push on the new segment
            }

            if(ltail->push(item,tid)) break; //exit successfully
            else{
                uint64_t currentSegmentTail = segmentTail.load(std::memory_order_acquire);
                uint64_t currentSegmentHead = segmentHead.load(std::memory_order_acquire);
                if(currentSegmentTail - currentSegmentHead >= MAX_SEGMENTS-1){
                    //Fail cause of full queue
                    HP.clear(kHpTail,tid);
                    return false;
                }
            }

            /**
             * Try to allocate a new segment
             * 
             * @note can be performed by more than one thread if the remaining segments are more than one
             */

            Segment* newTail = new Segment(sizeRing,0,ltail->getTailIndex()-1);
            newTail->push(item,tid);    //push on the new local segment

            Segment* nullSegment = nullptr;
            if(ltail->next.compare_exchange_strong(nullSegment,newTail)){//if CAS successful then alloc successful
                HP.protect(kHpTail,newTail,tid); //update protection on new tail
                tail.compare_exchange_strong(ltail,newTail);    //this always success 
                segmentTail.fetch_add(1,std::memory_order_release); //update the segmentTail
                uint64_t currentSegmentTail = segmentTail.load(std::memory_order_acquire);
                uint64_t currentSegmentHead = segmentHead.load(std::memory_order_acquire);
                if( currentSegmentTail - currentSegmentHead >= MAX_SEGMENTS){   //account for incremented tail
                    while(!tail.load(std::memory_order_acquire)->closeSegment()){
                        //spin until the segment is closed;
                    }
                }
                break;  //insertion successful on new segment
            } 
            else delete newTail; //another thread already updated the shared tail
            
            ltail = HP.protect(kHpTail,nullSegment,tid);    //update protection on the current new segment
        }
        HP.clear(kHpTail,tid);  //clear protection on the current segment
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
        T* item = nullptr;
        //protect the current global head upon entering
        Segment* lhead = HP.protect(kHpHead,head.load(std::memory_order_acquire),tid);
        while(true){
#ifndef DISABLE_HAZARD
            //check for local head consistency
            Segment *lhead2 = head.load(std::memory_order_acquire);
            if(lhead2 != lhead){
                lhead = HP.protect(kHpHead,lhead2,tid);
                continue;
            }           
#endif
            item = lhead->pop(tid);
            if(item != nullptr) break;  //break successfully

            //if unsuccesful pop then try to load next segment
            Segment* lnext = lhead->next.load(std::memory_order_acquire);
            if (lnext == nullptr) break; //break on failure [empty queue]

            /**
             * Try to see if ater first pop and lnext set somebody else still inserted in the queue
             */
            item = lhead->pop(tid);
            if(item != nullptr) break;

            //if pop unsuccessful then the closedBit has been set and it's guaranteed no further
            //insertions in the current segment

            //try to update the global head
            if (head.compare_exchange_strong(lhead, lnext)) {
                Segment* oldHead = lhead;
                lhead = HP.protect(kHpHead, lnext, tid); //protect the new head [drops protection on the old one]
                /**
                 * retire the old head
                 * @warning retire doesn't deallocate the segment if still in use by other threads
                 * @note we work on the assumption that segments will be eventually deallocated
                 */
                HP.retire(oldHead,tid);

                //update headSegment
                segmentHead.fetch_add(1,std::memory_order_release);  
            } else lhead = HP.protect(kHpHead, lhead, tid);
            
        }

        HP.clear(kHpHead,tid); //after pop removes protection on the current segment
        return item;
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