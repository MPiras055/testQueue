#pragma once

#include <atomic>
#include <cassert>

#include "LinkedRingQueue.hpp"
#include "BoundedElementCounter.hpp"
#include "RQCell.hpp"
#include "x86Atomics.hpp"


template <typename T, bool padded_cells>
class CRQueue : public QueueSegmentBase<T, CRQueue<T, padded_cells>> {
private:

    using Base = QueueSegmentBase<T, CRQueue<T, padded_cells>>;
    using Cell = detail::CRQCell<T *, padded_cells>;

    size_t sizeRing;
#ifndef DISABLE_POW2
    size_t mask;  //Mask to execute the modulo operation
#endif
    Cell *array;

    /** 
     * @brief returns the index of the current node [without the MSB]
     * */
    inline uint64_t nodeIndex(uint64_t i) const{
        return (i & ~(1ull << 63));
    }
    /** 
     * @brief checks if a node is unsafe
     * */
    inline bool nodeUnsafe(uint64_t i) const {
        return i & (1ull << 63);
    }
    /** 
     * @brief sets the unsafe bit of a given index 
     */
    inline uint64_t setUnsafe(uint64_t i) const {
        return (i | (1ull << 63));
    }

private:
    /**
     * @brief Private Constructor for CRQ segment
     * 
     * @param size_par (size_t) sizeRing of the segment
     * @param tid (int) thread id [not used]
     * @param start (uint64_t) start index of the segment [useful for LinkedRing Adaptation]
     * 
     * @note the `tid` parameters is not used but is kept for compatibility with the LinkedRingQueue
     */
    CRQueue(size_t size_par,[[maybe_unused]] const int tid, const uint64_t start): Base(), 
#ifndef DISABLE_POW2
    sizeRing{detail::isPowTwo(size_par) ? size_par : detail::nextPowTwo(size_par)},
    mask{sizeRing - 1}
#else
    sizeRing{size_par}
#endif
    {
        assert(size_par > 0);
        array = new Cell[sizeRing];

        for(uint64_t i = start; i < start + sizeRing; ++i){
            array[i % sizeRing].val.store(nullptr,std::memory_order_relaxed);
            array[i % sizeRing].idx.store(i,std::memory_order_relaxed);           
        }

        Base::head.store(start,std::memory_order_relaxed);
        Base::tail.store(start,std::memory_order_relaxed);
    }


public: 
    /**
     * @brief Constructor for CRQ segment
     * 
     * @param size_par (size_t) sizeRing of the segment
     * @param tid (int) thread id [not used]
     * 
     * Constructs a new CRQ bounded segment with given sizeRing
     * 
     * @note the `tid` parameter is not used but is kept for compatibility with the LinkedRingQueue
     */
    CRQueue(size_t size_par,[[maybe_unused]] const int tid = 0): CRQueue(size_par,tid,0){}

    /**
     * @brief Destructor for CRQ segment
     * 
     * Deletes the underlying array of the segment after draining it
     */
    ~CRQueue() { 
        while(pop(0) != nullptr);
        delete[] array; 
    }

    static std::string className(bool padding = true){
        using namespace std::string_literals;
        return "CRQueue"s + ((padded_cells && padding)? "/padded":"");
    }

    /**
     * @brief Push operation for CRQ
     * 
     * @param item (T*) item to push
     * @param tid (int) thread id [not used]
     * 
     * Pushes an item into the queue if there's space else returns false
     * 
     * @returns (bool) true if the operation is successful
     * 
     * @note the `tid` parameter is not used but is kept for compatibility with the LinkedRingQueue
     * @note if queue is instantiated as bounded then the segment never closes
     * @warning uses 2CAS operation implemented as asm routines in x86Atomics.hpp
     */
    __attribute__((used,always_inline)) bool push(T *item,[[maybe_unused]] const int tid = 0)
    {
        while (true)
        {
            uint64_t tailTicket = Base::tail.fetch_add(1,std::memory_order_acquire);

            
            if(Base::isClosed(tailTicket)){
                return false;
            }
            
#ifndef DISABLE_POW2
            Cell &cell = array[tailTicket & mask];
#else
            Cell &cell = array[tailTicket % sizeRing];
#endif
            uint64_t idx = cell.idx.load();
            if (cell.val.load() == nullptr )
            {
                if (nodeIndex(idx) <= tailTicket)
                {
                    if ((!nodeUnsafe(idx) || Base::head.load() < tailTicket))
                    {
                        if (CAS2((void **)&cell, nullptr, idx, item, tailTicket))
                            return true;
                    }
                }
            }

            if(tailTicket >= Base::head.load() + sizeRing)
            {
                Base::closeSegment(tailTicket);
                return false;
            }
        }
    }

    /***
     * @brief Pop operation for CRQ
     * 
     * @param tid (int) thread id [not used]
     * 
     * @returns (T*) item popped from the queue [nullptr if the queue is empty]
     * 
     * @note the `tid` parameter is not used but is kept for compatibility with the LinkedRingQueue
     * @warning uses 2CAS operation implemented as asm routines in x86Atomics.hpp
     */
    __attribute__((used,always_inline)) T *pop([[maybe_unused]] const int tid = 0)
    {
#ifdef CAUTIOUS_DEQUEUE //checks if the queue is empty before trying operations
        if (Base::isEmpty()) return nullptr;
#endif
        while (true)
        {
            uint64_t headTicket = Base::head.fetch_add(1,std::memory_order_acquire);
#ifndef DISABLE_POW2
            Cell &cell = array[headTicket & mask];
#else
            Cell &cell = array[headTicket % sizeRing];
#endif

            int r = 0;
            uint64_t tt = 0;

            while (true)
            {
                uint64_t cell_idx = cell.idx.load();
                uint64_t unsafe = nodeUnsafe(cell_idx);
                uint64_t idx = nodeIndex(cell_idx);
                T *val = static_cast<T *>(cell.val.load());

                if (idx > headTicket) break;

                if (val != nullptr)
                {   //Dequeue transition
                    if (idx == headTicket)
                    {
                        if (CAS2((void **)&cell, val, cell_idx, nullptr, unsafe | (headTicket + sizeRing)))
                            return val;
                    }
                    else
                    {//Unsafe transition [headTicket > idx]
                        if (CAS2((void **)&cell, val, cell_idx, val, setUnsafe(idx)))
                            break;
                    }
                }
                else
                {//Empty cell (Advance the index to next epoch)
                    if ((r & ((1ull << 8) - 1)) == 0)
                        tt = Base::tail.load();

                    int closed = Base::isClosed(tt);
                    uint64_t t = Base::tailIndex(tt);
                    if (unsafe || t < headTicket + 1 || closed || r > 4 * 1024 )
                    {
                        if (CAS2((void **)&cell, val, cell_idx, val, unsafe | (headTicket + sizeRing)))
                            break;
                    }
                    ++r;
                }
            }

            if (Base::tailIndex(Base::tail.load()) <= headTicket)
            {
                Base::fixState();
                return nullptr; //empty queue
            }
        }
    }

    /**
     * @brief returns the length of the queue
     * 
     * @param tid (int) thread id [not used]
     * 
     * @note the `tid` parameter is not used but is kept for compatibility with the LinkedRingQueue
     */
    inline size_t length([[maybe_unused]] const int tid = 0) const {
        return Base::length();
    }

    /**
     * @brief returns the size of the queue
     * 
     * @returns (size_t) size of the queue
     * @note it's useful when POW2 is disabled since the size attribute can differ from the parameter
     */
    inline size_t capacity() const {
        return sizeRing;
    }

    friend class LinkedRingQueue<T,CRQueue<T,padded_cells>>;   //LinkedRingQueue can access private class members 
    friend class BoundedLinkedAdapter<T,CRQueue<T,padded_cells>>; //BoundedLinkedAdapter can access private class members
};

/**
 * Alias for easy instantiation
 */
template <typename T>
#ifdef DISABLE_PADDING
using BoundedCRQueue = BoundedLinkedAdapter<T, CRQueue<T, false>>;
template <typename T>
using LCRQueue = LinkedRingQueue<T, CRQueue<T, false>>;
#else
using BoundedCRQueue = BoundedLinkedAdapter<T, CRQueue<T, true>>;
template <typename T>
using LCRQueue = LinkedRingQueue<T, CRQueue<T, true>>;
#endif