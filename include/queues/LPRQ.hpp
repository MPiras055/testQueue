#pragma once

#include "LinkedAdapter.hpp"
#include "BoundedSegmentAdapter.hpp"
#include "BoundedItemAdapter.hpp"
#include "RQCell.hpp"
#include "x86Atomics.hpp"

#ifndef TRY_CLOSE_PRQ
#define TRY_CLOSE_PRQ 10
#endif

template<typename T,bool padded_cells>
class PRQueue : public QueueSegmentBase<T, PRQueue<T,padded_cells>> {
private:
    using Base = QueueSegmentBase<T,PRQueue<T,padded_cells>>;
    using Cell = detail::CRQCell<void*,padded_cells>;

    static constexpr size_t TRY_CLOSE = 10;
    
    Cell* array; 
    const size_t size;
#ifndef DISABLE_POW2
    const size_t mask;  //Mask to execute the modulo operation
#endif

    /* Private class methods */
    inline uint64_t nodeIndex(uint64_t i) const {return (i & ~(1ull << 63));}
    inline uint64_t setUnsafe(uint64_t i) const {return (i | (1ull << 63));}
    inline uint64_t nodeUnsafe(uint64_t i) const {return (i & (1ull << 63));}
    inline bool isBottom(void* const value) const {return (reinterpret_cast<uintptr_t>(value) & 1) != 0;}
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

        for(uint64_t i = start; i < start + size; ++i){
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
    __attribute__((used,always_inline)) bool push(T* item,[[maybe_unused]] const int tid = 0) {
        int try_close = 0;
    
        while(true) {

            uint64_t tailTicket = Base::tail.fetch_add(1);
            if(Base::isClosed(tailTicket)) {
                return false;
            }
            
#ifndef DISABLE_POW2
            Cell& cell = array[tailTicket & mask];
#else
            Cell& cell = array[tailTicket % size];
#endif
            uint64_t idx = cell.idx.load();
            void* val = cell.val.load();

            if( val == nullptr
                && nodeIndex(idx) <= tailTicket
                && (!nodeUnsafe(idx) || Base::head.load() <= tailTicket)) 
            {
                void* bottom = threadLocalBottom(tid);
                if(cell.val.compare_exchange_strong(val,bottom)) {
                    if(cell.idx.compare_exchange_strong(idx,tailTicket + size)) {
                        if(cell.val.compare_exchange_strong(bottom, item)) {
                            return true;
                        }
                    } else {
                        cell.val.compare_exchange_strong(bottom, nullptr);
                    }
                }
            }

            if(tailTicket >= Base::head.load() + size){
                if (Base::closeSegment(tailTicket,try_close++ < TRY_CLOSE_PRQ))
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
#ifdef CAUTIOUS_DEQUEUE
        if(Base::isEmpty())
            return nullptr;
#endif
        while(true) {

            uint64_t headTicket = Base::head.fetch_add(1);
#ifndef DISABLE_POW2
            Cell& cell = array[headTicket & mask];
#else
            Cell& cell = array[headTicket % size];
#endif

            int r = 0;
            uint64_t tt = 0;

            while(1) {
                uint64_t cell_idx   = cell.idx.load();
                uint64_t unsafe     = nodeUnsafe(cell_idx);
                uint64_t idx        = nodeIndex(cell_idx);

                void* val           = cell.val.load();

                if(val != nullptr && !isBottom(val)){
                    if(idx == headTicket + size){
                        cell.val.store(nullptr);
                        return static_cast<T*>(val);
                    } else {
                        if(unsafe) {
                            if(cell.idx.load() == cell_idx)
                                break;
                        } else {
                            if(cell.idx.compare_exchange_strong(cell_idx,setUnsafe(idx)))
                                break;
                        }
                    }
                } else {
                    if((r & ((1ull << 8 ) -1 )) == 0)
                        tt = Base::tail.load();

                    int closed = Base::isClosed(tt);    //in case "bounded" it's always false
                    uint64_t t = Base::tailIndex(tt);
                    if(unsafe || t < headTicket + 1  || r > 4 * 1024 || closed) {
                        if(isBottom(val) && !cell.val.compare_exchange_strong(val,nullptr))
                            continue;
                        if(cell.idx.compare_exchange_strong(cell_idx, unsafe | (headTicket + size)))
                            break;
                    }
                    ++r;
                }
            }

            if(Base::tailIndex(Base::tail.load()) <= headTicket + 1){
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