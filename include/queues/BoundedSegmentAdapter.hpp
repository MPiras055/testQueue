#pragma once
#include "HazardPointers.hpp"
#include <stdexcept>
#include <iostream>
#include "Segment.hpp"
#include "RQCell.hpp"

#ifndef CACHE_LINE
#define CACHE_LINE 64
#endif

template<class T, class Segment>
class BoundedSegmentAdapter {
private:
    static constexpr size_t MAX_THREADS = HazardPointers<Segment*>::MAX_THREADS;
    static constexpr size_t MAX_SEGMENTS = 4;
    static constexpr int kHpTail = 0;
    static constexpr int kHpHead = 1;
    
    const size_t maxSegments;
    const size_t sizeRing;
    const size_t maxThreads;

    alignas(CACHE_LINE) std::atomic<Segment*> head;
    alignas(CACHE_LINE) std::atomic<Segment*> tail;
    alignas(CACHE_LINE) std::atomic<uint64_t> segmentHeadIdx;
    alignas(CACHE_LINE) std::atomic<uint64_t> segmentTailIdx;

    /**
     * upon failed push each thread sets the skip_push variable that gets reset when the current segment
     * is changed. If the segment remains the same, before attempting further push it's checked if the current
     * segment is closed
     * 
     * @note mantains its state between push/pops operations
     */
    alignas(CACHE_LINE) static inline thread_local bool skip_push = false;

    HazardPointers<Segment> HP;

public:
    BoundedSegmentAdapter(size_t size_par, size_t threads = MAX_THREADS, size_t segmentCount = 4):
    maxSegments{segmentCount},
#ifndef DISABLE_POW2
    sizeRing{(detail::isPowTwo(size_par)? size_par : detail::nextPowTwo(size_par)) / maxSegments},
#else 
    sizeRing{size_par},
#endif
    maxThreads{threads},
    HP(2,maxThreads)
    {
#ifndef DISABLE_HAZARD
        assert(maxThreads <= MAX_THREADS);
#endif
        assert(sizeRing > 0);

        Segment* sentinel = new Segment(sizeRing);
        head.store(sentinel, std::memory_order_relaxed);
        tail.store(sentinel, std::memory_order_relaxed);

        segmentHeadIdx.store(0, std::memory_order_relaxed);
        segmentTailIdx.store(0, std::memory_order_relaxed);
    }

    ~BoundedSegmentAdapter() {
        while(pop(0) != nullptr);
        delete head.load();
    }

    static std::string className(bool padding = true){
        return "BoundedSegment" + Segment::className(padding);
    }


    __attribute__((used,always_inline)) bool push(T* item, int tid) {
        Segment* ltail = HP.protect(kHpTail, tail.load(), tid);
        while (true) {

#ifndef DISABLE_HP
            //check for tail inconsistency
            /**
             * updates protection on the current global head
             * if it's changed after last cycle
             */
            Segment* ltail2 = tail.load();
            if (ltail2 != ltail) {
                skip_push = false;
                ltail = HP.protect(kHpTail, ltail2, tid);
                continue;
            }
#endif

            /**
             * if a new segment is present then it tries to advance the global head
             */
            Segment *lnext = ltail->next.load();
            //advance the global head
            if (lnext != nullptr) {
                if(tail.compare_exchange_strong(ltail, lnext)){
                    ltail = HP.protect(kHpTail, lnext, tid);
                }
                else{
                    ltail = HP.protect(kHpTail, tail.load(), tid);
                }
                skip_push = false;  //is segment is changed then we perform an "exploratory" push
                continue;
            }
            
            /**
             * If skip_push is set to true we don't perform push operation if the current segment is closed
             * 
             * If the push operation on the current segment fails then skip_push is set to true until the segment
             * reference is changed (via segment update) 
             */

            if(skip_push)
                skip_push = ltail->isClosed();

            if(!skip_push){
                if(ltail->push(item,tid)){
                    HP.clear(kHpTail,tid);
                    return true;
                } else skip_push = true;
            }
            

            // checks if the following allocation doesn't exceed the max limit
            if(segmentCount() > maxSegments){
                HP.clear(kHpTail,tid);
                return false;
            }

            Segment* newTail = new Segment(sizeRing,maxThreads,ltail->getNextSegmentStartIndex());
            newTail->push(item, tid);

            Segment* nullNode = nullptr;

            //The segment update is not made in mutual exclusion to ensure performance (the memory bound increases)
            if (ltail->next.compare_exchange_strong(nullNode, newTail)) {
                tail.compare_exchange_strong(ltail, newTail);
                segmentTailIdx.fetch_add(1,std::memory_order_release);
                skip_push = false;
                HP.clear(kHpTail, tid);
                return true;
            }

            delete newTail;
            ltail = HP.protect(kHpTail, nullNode, tid);
            skip_push = false;
        }
    }

        __attribute__((used,always_inline)) T* pop(int tid) {
        Segment* lhead = HP.protect(kHpHead, head.load(), tid);
        while (true) {
#ifndef DISABLE_HP
            //check for head inconsistency
            Segment* lhead2 = head.load();
            if (lhead2 != lhead) {
                lhead = HP.protect(kHpHead, lhead2, tid);
                continue;
            }
#endif
            //try to pop from the current segment
            T* item = lhead->pop(tid);
            if (item != nullptr){
                HP.clear(kHpHead, tid);
                return item;
                
            }

            Segment* lnext = lhead->next.load();
            if (lnext != nullptr){
                item = lhead->pop(tid);
                if (item == nullptr) {
                    //When changing head segment
                    if (head.compare_exchange_strong(lhead, lnext)) {
                        segmentHeadIdx.fetch_add(1,std::memory_order_release);
                        HP.retire(lhead, tid);
                        lhead = HP.protect(kHpHead, lnext, tid);
                    } else {  
                        lhead = HP.protect(kHpHead, lhead, tid); 
                    }  
                    continue;
                }

                HP.clear(kHpHead, tid);
                return item;
            }
            
            //queue apperas empty
            HP.clear(kHpHead, tid);
            return nullptr;       
        }
    }

    size_t length(int tid) {
        Segment *lhead = HP.protect(kHpHead,head,tid);
        Segment *ltail = HP.protect(kHpTail,tail,tid);
        uint64_t t = ltail->getTailIndex();
        uint64_t h = lhead->getHeadIndex();
        HP.clear(tid);
        return t > h ? t - h : 0;
    }

    inline size_t capacity() const {
        return sizeRing;
    }

private:
    __attribute__((used,always_inline)) inline size_t segmentCount(){
        //if the threads are on the same segments we still count one segment allocated
        return segmentTailIdx.load(std::memory_order_relaxed) - segmentHeadIdx.load(std::memory_order_relaxed) + 1;
    }

};