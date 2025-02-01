#pragma once
#include "LinkedAdapter.hpp"
#include "BoundedSegmentAdapter.hpp"
#include "BoundedItemAdapter.hpp"
#include "NumaDispatcher.hpp"
#include "RQCell.hpp"

#ifndef TRY_CLOSE_PRQ
#define TRY_CLOSE_PRQ 10
#endif

template<typename T,bool padded_cells>
class PRQueue : public QueueSegmentBase<T, PRQueue<T,padded_cells>> {
private:
    using Base = QueueSegmentBase<T,PRQueue<T,padded_cells>>;
    using Cell = detail::CRQCell<void*,padded_cells>;
    
    static inline thread_local int threadCluster = NumaDispatcher::get_numa_node();

    Cell* array; 
    const size_t size;
#ifndef DISABLE_POW2
    const size_t mask;  //Mask to execute the modulo operation
#endif

    /* Private class methods */
    inline uint64_t nodeIndex(uint64_t i) const {return (i & ~(1ull << 63));}
    inline uint64_t setUnsafe(uint64_t i) const {return (i | (1ull << 63));}
    inline uint64_t nodeUnsafe(uint64_t i) const {return (i & (1ull << 63));}

    /**
     * The lsb of a valid pointer is always 0 a ptr it's valid if the lsb is 0
     * 
     * consumers check if the value is a bottom pointer to know if the cell contains a significant value
     * if not they don't extract so they don't break the queue flow
     */
    inline bool isBottom(void* const value) const {
        return (reinterpret_cast<uintptr_t>(value) & 1) != 0;
    }

    inline void* threadLocalBottom(const int tid) const {
        return reinterpret_cast<void*>(static_cast<uintptr_t>((tid << 1) | 1));
    }

private:
    //uses the tid argument to be consistent with linked queues
    PRQueue(size_t size_par, [[maybe_unused]] const int tid, const uint64_t start): Base(),
#ifndef DISABLE_POW2
    size{detail::isPowTwo(size_par)? size_par : detail::nextPowTwo(size_par)},
    mask{size - 1}
#else
    size{size_par}
#endif
    {
        assert(size_par > 0);
        array = new Cell[size];

        for(uint64_t i = start; i < start + size; i++){
            array[i % size].val.store(nullptr,std::memory_order_relaxed);
            array[i % size].idx.store(i,std::memory_order_relaxed);           
        }

        Base::head.store(start,std::memory_order_relaxed);
        Base::tail.store(start,std::memory_order_relaxed);
    }

public:
    //uses the tid argument to be consistent with linked queues
    PRQueue(size_t size_par, [[maybe_unused]] const int tid = 0): PRQueue(size_par,tid,0){}

    ~PRQueue(){
        while(pop(0) != nullptr);
        delete[] array;
    }

    static std::string className(bool padding = true) {
        using namespace std::string_literals;
        return "PRQueue"s + ((padded_cells && padding)? "/padded":"");
    }

    /*
        Takes an additional tid parameter to keep the interface compatible with
        LinkedRingQueue->push
        The parameter has a default value so that it can be omitted
    */
    __attribute__((used,always_inline)) bool push(T* item, const int tid) {
        Base::waitOnCluster(threadCluster);

        int try_close = 0;

        while (true) {
            uint64_t tailticket = Base::tail.fetch_add(1);
            if (Base::isClosed(tailticket)) {
                return false;
            }

            Cell& cell = array[tailticket % size];
            uint64_t idx = cell.idx.load();
            void* val   = cell.val.load();
            if (val == nullptr){
                if(nodeIndex(idx) <= tailticket){
                    if((!nodeUnsafe(idx) || Base::head.load() <= tailticket)) {
                        void* bottom = threadLocalBottom(tid);
                        if (cell.val.compare_exchange_strong(val, bottom)) {
                            if (cell.idx.compare_exchange_strong(idx, tailticket + size)) {
                                if (cell.val.compare_exchange_strong(bottom, item)) {
                                    return true;
                                }
                            } else {
                                cell.val.compare_exchange_strong(bottom, nullptr);
                            }
                        }
                    }
                }
            }
            if (tailticket >= Base::head.load() + size) {
                if (Base::closeSegment(tailticket, try_close > TRY_CLOSE_PRQ))
                    return false;
            }
        }
    }

    /*
        Takes an additional tid parameter to keep the interface compatible with
        LinkedRingQueue->push
        The parameter has a default value so that it can be omitted
    */
    __attribute__((used,always_inline)) T* pop([[maybe_unused]] const int tid = 0) {
        Base::waitOnCluster(threadCluster);
#ifdef CAUTIOUS_DEQUEUE
        if (Base::isEmpty())
            return nullptr;
#endif

        while (true) {
            uint64_t headticket = Base::head.fetch_add(1);
            Cell& cell = array[headticket % size];

            int r = 0;
            uint64_t tt = 0;

            while (true) {
                uint64_t cell_idx = cell.idx.load();
                uint64_t unsafe = nodeUnsafe(cell_idx);
                uint64_t idx = nodeIndex(cell_idx);
                void* val = cell.val.load();

                if (idx > (headticket + size))
                    break;

                if ((val != nullptr) && !isBottom(val)) {
                    if (idx == (headticket + size)) {   //only one dequeuer will pass this condition
                        cell.val.store(nullptr);
                        return static_cast<T*>(val);
                    } else {
                        if (unsafe) {
                            if (cell.idx.load() == cell_idx)
                                break;
                        } else {
                            if (cell.idx.compare_exchange_strong(cell_idx, setUnsafe(idx)))
                                break;
                        }
                    }
                } else {
                    //every tot iterations checks if the queue is closed
                    if ((r & ((1ull << 8) - 1)) == 0)
                        tt = Base::tail.load(); //every 256 retries

                    int crq_closed = Base::isClosed(tt);
                    uint64_t t = Base::tailIndex(tt);
                    /**
                     * unsafe cell (dequeuers are not dequeuing | could be a bottom pointer)
                     * the tail is behind (high contention)
                     * the queue is closed
                     * thread spinning for too long
                     */
                    if (unsafe || t < (headticket + 1) || crq_closed || (r > 4*1024)) {
                        //if the value is a bottom pointer, that needs to be cleared (try to clean)
                        if (isBottom(val) && !cell.val.compare_exchange_strong(val, nullptr))
                            continue;
                        //if the cell is unsafe then it's shifted to the next epoch (another dequeuer dequeue will try to fix it)
                        if (cell.idx.compare_exchange_strong(cell_idx, unsafe | (headticket + size))){
                            break;
                        }
                    }
                    ++r;    //increment the retry counter
                }
            }

            /**
             * when exiting check if the indexes overlapped
             */
            if (Base::tailIndex(Base::tail.load()) <= headticket + 1) { //ogni volta che il consumatore esce
                Base::fixState();
                return nullptr;
            }
        }
    
    }

    inline size_t length([[maybe_unused]] const int tid = 0) const {
        return Base::length();
    }

    friend class LinkedAdapter<T,PRQueue<T,padded_cells>>;   
    friend class BoundedItemAdapter<T,PRQueue<T,padded_cells>>;
    friend class BoundedSegmentAdapter<T,PRQueue<T,padded_cells>>;
  
};

/*
    Declare aliases for Unbounded and Bounded Queues
*/




template <typename T>
#ifdef DISABLE_PADDING
using BoundedSegmentPRQueue = BoundedSegmentAdapter<T, PRQueue<T, false>>;
template<typename T>
using BoundedItemPRQueue = BoundedItemAdapter<T,PRQueue<T,false>>;
template <typename T>
using LPRQueue = LinkedAdapter<T, PRQueue<T, false>>;
#else
using BoundedSegmentPRQueue = BoundedSegmentAdapter<T, PRQueue<T, true>>;
template <typename T>
using BoundedItemPRQueue = BoundedItemAdapter<T,PRQueue<T,true>>;
template <typename T>
using LPRQueue = LinkedAdapter<T, PRQueue<T, true>>;
#endif