#pragma once
#include "HazardPointers.hpp"
#include <stdexcept>
#include <iostream>
#include "Segment.hpp"
#include "RQCell.hpp"


#ifndef CACHE_LINE
#define CACHE_LINE 64ul
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
    const size_t fullLength;

    alignas(CACHE_LINE) std::atomic<Segment*> head;
    alignas(CACHE_LINE) std::atomic<Segment*> tail;
    alignas(CACHE_LINE) std::atomic<uint64_t> segmentHeadIdx;
    alignas(CACHE_LINE) std::atomic<uint64_t> segmentTailIdx;

    /**
     * upon failed push each thread sets the check_push variable that gets reset when the current segment
     * is changed. If the segment remains the same, before attempting further push it's checked if the current
     * segment is closed
     * 
     * @note mantains its state between push/pops operations
     */
    alignas(CACHE_LINE) static inline thread_local bool check_push = false;

    HazardPointers<Segment> HP;

public:
    BoundedSegmentAdapter(size_t size_par, size_t threads = MAX_THREADS, size_t segmentCount = MAX_SEGMENTS):
#ifndef DISABLE_POW2
    maxSegments{segmentCount > 1 && detail::isPowTwo(segmentCount)? segmentCount : detail::nextPowTwo(segmentCount)},
    sizeRing{( detail::isPowTwo(size_par)? size_par : detail::nextPowTwo(size_par)) / maxSegments},
#else 
    sizeRing{size_par},
    maxSegments{segmentCount},
#endif
    maxThreads{threads},
    fullLength{sizeRing * maxSegments},
    HP(2,maxThreads)
    {
#ifdef DEBUG
#ifndef DISABLE_HAZARD
    assert(maxThreads <= MAX_THREADS);
#endif
    assert(sizeRing != 0);
    assert(maxSegments != 0);
#endif

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
        Segment* ltail = HP.protectRelease(kHpTail, tail.load(std::memory_order_acquire), tid);
        while (true) {
#ifndef DISABLE_HP
            //check for tail inconsistency
            /**
             * updates protectReleaseion on the current global head
             * if it's changed after last cycle
             */
            Segment* ltail2 = tail.load(std::memory_order_acquire);
            if (ltail2 != ltail) {
                check_push = false;
                ltail = HP.protectRelease(kHpTail, ltail2, tid);
                continue;
            }
#endif

            /**
             * if a new segment is present then it tries to advance the global head
             */
            Segment *lnext = ltail->next.load(std::memory_order_acquire);
            //advance the global head
            if (lnext != nullptr) {
                if(tail.compare_exchange_strong(ltail, lnext,std::memory_order_acq_rel)){
                    ltail = HP.protectRelease(kHpTail, lnext, tid);
                }
                else{
                    ltail = HP.protectRelease(kHpTail, tail.load(std::memory_order_acquire), tid);
                }
                check_push = false;  //is segment is changed then we perform an "exploratory" push
                continue;
            }
            
            /**
             * If check_push is set to true we don't perform push operation if the current segment is closed
             * 
             * If the push operation on the current segment fails then check_push is set to true until the segment
             * reference is changed (via segment update) 
             */

            if(check_push) check_push = ltail->isClosed();

            if(!check_push){
                if(ltail->push(item,tid)){
                    HP.clear(kHpTail,tid);
                    return true;
                } else check_push = true;
            }
            
            // checks if the following allocation doesn't exceed the max limit [set by acquire_release]
            if(ltail->next.load(std::memory_order_relaxed) != nullptr)
                continue;

            if(segmentCount() >= maxSegments){
                HP.clear(kHpTail,tid);
                return false;
            }

            Segment* newTail = new Segment(sizeRing,maxThreads,ltail->getNextSegmentStartIndex());
            newTail->push(item, tid);

            Segment* nullNode = nullptr;

            //The segment update is not made in mutual exclusion to ensure performance (the memory bound increases)
            if (ltail->next.compare_exchange_strong(nullNode, newTail, std::memory_order_acq_rel)) {
                tail.compare_exchange_strong(ltail, newTail, std::memory_order_acq_rel);
                segmentTailIdx.fetch_add(1,std::memory_order_release);
                check_push = false;
                HP.clear(kHpTail, tid);
                return true;
            }

            delete newTail;
            ltail = HP.protectRelease(kHpTail, nullNode, tid);
            check_push = false;
        }
    }

    __attribute__((used,always_inline)) T* pop(int tid) {
        Segment* lhead = HP.protectRelease(kHpHead, head.load(std::memory_order_acquire), tid);

        T* item = nullptr;
        while (true) {
#ifndef DISABLE_HP
            //check for head inconsistency
            Segment* lhead2 = head.load(std::memory_order_acquire);
            if (lhead2 != lhead) {
                lhead = HP.protectRelease(kHpHead, lhead2, tid);
                continue;
            }
#endif
            //try to pop from the current segment
            if (item = lhead->pop(tid); item != nullptr) break;

            Segment* lnext = lhead->next.load(std::memory_order_acquire);
            if (lnext == nullptr) break;

            if (item = lhead->pop(tid); item != nullptr) break;
            
            //When changing head segment
            if (head.compare_exchange_strong(lhead, lnext, std::memory_order_acq_rel)) {
                segmentHeadIdx.fetch_add(1,std::memory_order_release);
                HP.retire(lhead, tid);
                lhead = HP.protectRelease(kHpHead, lnext, tid);
            } else
                lhead = HP.protectRelease(kHpHead, lhead, tid); 
            
            
        }

        //queue appears empty
        HP.clear(kHpHead, tid);
        return item;    
    }

    size_t length([[maybe_unused]] const int tid = 0) {
        Segment *lhead = HP.protectRelease(kHpHead,head,tid);
        Segment *ltail = HP.protectRelease(kHpTail,tail,tid);
        uint64_t t = ltail->tailIndex();
        uint64_t h = lhead->headIndex();
        HP.clear(tid);
        return t > h ? t - h : 0;
    }

    inline size_t capacity() const {
        return sizeRing * maxSegments;
    }

private:
    __attribute__((used,always_inline)) inline size_t segmentCount() const {
        //if the threads are on the same segments we still count one segment allocated
        return segmentTailIdx.load(std::memory_order_acquire) - segmentHeadIdx.load(std::memory_order_acquire) + 1;
    }

};