#pragma once

#include "LinkedAdapter.hpp"
#include "BoundedSegmentAdapter.hpp"
#include "BoundedItemAdapter.hpp"

#include "CacheRemap.hpp"
#include "RQCell.hpp"
#include "x86Atomics.hpp"

//max retries for closing segment [assuming spurious fails]
#ifndef TRY_CLOSE_CRQ 
#define TRY_CLOSE_CRQ 10
#endif

#ifndef CACHE_LINE
#define CACHE_LINE 64
#endif

template <typename T, bool padded_cells>
class CRQueue : public QueueSegmentBase<T, CRQueue<T, padded_cells>> {
private:
    using Base = QueueSegmentBase<T, CRQueue<T, padded_cells>>;
    using Cell = detail::CRQCell<T *, padded_cells>;

    /**
     * @brief thread_local variable to store the current numa node
     * 
     * This variable is useful when performing in_cluster optimization because
     * threads are pinned to a specific numa node
     * 
     * @note the variable is initialized to the current numa node
     * @note gets the numa node of the cpu where the thread is currently executing
     */

    const size_t sizeRing;
#ifndef DISABLE_POW2
    const size_t mask;  //Mask to execute the modulo operation
#endif

    [[no_unique_address]] const CacheRemap<sizeof(Cell), CACHE_LINE> remap;
    Cell *array;

    /** 
     * @brief extracts the index (63 LSB) from a given value
     * */
    inline uint64_t node_index(uint64_t i) const { return (i & ~(1ull << 63));}
    /** 
     * @brief extracts the MSB of a value
     * */
    inline uint64_t node_unsafe(uint64_t i) const { return (i & (1ull << 63));}
    /** 
     * @brief sets the MSB of a value to 1
     */
    inline uint64_t set_unsafe(uint64_t i) const { return (i | (1ull << 63)); }

public:
    /**
     * @brief Private Constructor for CRQ segment
     * 
     * @param size_par (size_t) sizeRing of the segment
     * @param tid (int) thread id [not used]
     * @param start (uint64_t) start index of the segment [useful for LinkedRing Adaptation]
     * 
     * @note the `tid` parameters is not used but is kept for compatibility with the LPRQ
     */
    CRQueue(size_t size_par,[[maybe_unused]] const int tid, const uint64_t start): Base(), 
#ifndef DISABLE_POW2
    //we could want to initialize a segment as 0 as a sentinel
    sizeRing{(size_par != 1) && detail::isPowTwo(size_par) ? size_par : detail::nextPowTwo(size_par)}, //round up to next power of 2
    mask{sizeRing - 1},  //sets the mask to perform modulo operation
#else
    sizeRing{size_par},
#endif
    remap{CacheRemap<sizeof(Cell),CACHE_LINE>(sizeRing)} //initialize the cache remap
    {
        array = new Cell[sizeRing];

        //sets the fields of the Cell given the startingIndex (default 0)
        for (uint64_t i = start; i < start + sizeRing; i++) {
#ifdef DISABLE_POW2
            array[remap[i % sizeRing]].val.store(nullptr, std::memory_order_relaxed);
            array[remap[i % sizeRing]].idx.store(i, std::memory_order_relaxed);
#else
            array[remap[i & mask]].val.store(nullptr, std::memory_order_relaxed);
            array[remap[i & mask]].idx.store(i, std::memory_order_relaxed);
#endif
            
        }
        Base::head.store(start, std::memory_order_relaxed);
        Base::tail.store(start, std::memory_order_relaxed);
    }
    
    /**
     * @brief Constructor for CRQ segment
     * 
     * @param size_par (size_t) sizeRing of the segment
     * @param tid (int) thread id [not used]
     * 
     * Constructs a new CRQ bounded segment with given sizeRing
     * 
     * @note the `tid` parameter is not used but is kept for compatibility with the LinkedAdapter
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
     * @note the `tid` parameter is not used but is kept for compatibility with the LPRQ
     * @note if queue is instantiated as bounded then the segment never closes
     * @warning uses 2CAS operation implemented as asm routines in x86Atomics.hpp
     */
    __attribute__((used,always_inline)) bool push(T *item,[[maybe_unused]] const int tid = 0)
    {
        int try_close = 0;

        while (true) {
            uint64_t tailticket = Base::tail.fetch_add(1);
            if (Base::isClosed(tailticket)) {
                return false;
            }
#ifdef DISABLE_POW2
            Cell& cell = array[remap[tailticket % sizeRing]];
#else
            Cell& cell = array[remap[tailticket & mask]];
#endif
            uint64_t idx = cell.idx.load();
            if (cell.val.load() == nullptr) {
                if (node_index(idx) <= tailticket) {
                    if ((!node_unsafe(idx) || Base::head.load() < tailticket)) {
                        if (CAS2((void**)&cell, nullptr, idx, item, tailticket)) {
                            return true;
                        }
                    }
                }
            }
            if (tailticket >= Base::head.load() + sizeRing) {
                if (Base::closeSegment(tailticket,++try_close > TRY_CLOSE_CRQ))
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
     * @note the `tid` parameter is not used but is kept for compatibility with the LinkedAdapter
     * @warning uses 2CAS operation implemented as asm routines in x86Atomics.hpp
     */
    __attribute__((used,always_inline)) T *pop([[maybe_unused]] const int tid = 0){
#ifdef CAUTIOUS_DEQUEUE
        if (Base::isEmpty())
            return nullptr;
#endif

        while (true) {
            uint64_t headticket = Base::head.fetch_add(1);
#ifdef DISABLE_POW2
            Cell& cell = array[remap[headticket % sizeRing]];
#else
            Cell& cell = array[remap[headticket & mask]];
#endif

            int r = 0;
            uint64_t tt = 0;

            while (true) {
                uint64_t cell_idx = cell.idx.load();
                uint64_t unsafe = node_unsafe(cell_idx);
                uint64_t idx = node_index(cell_idx);
                T* val = static_cast<T*>(cell.val.load());

                if (idx > headticket)
                    break;

                if (val != nullptr) {   //Dequeue transition
                    if (idx == headticket) {
                        if (CAS2((void**)&cell, val, cell_idx, nullptr, unsafe | (headticket + sizeRing))) {
                            return val;
                        }
                    } else {            //Unsafe transition
                        if (CAS2((void**)&cell, val, cell_idx, val, set_unsafe(idx)))
                            break;
                    }
                } else {
                    if ((r & ((1ull << 8) - 1)) == 0)
                        tt = Base::tail.load();

                    int crq_closed = Base::isClosed(tt);
                    uint64_t t = Base::tailIndex(tt);
                    if (unsafe || t < headticket + 1 || crq_closed || r > 4*1024) {
                        if (CAS2((void**)&cell, val, cell_idx, val, unsafe | (headticket + sizeRing)))
                            break;
                    }
                    ++r;
                }
            }

            if (Base::tailIndex(Base::tail.load()) <= headticket + 1) {
                Base::fixState();
                return nullptr;
            }
        }
            
    }

    /**
     * @brief returns the length of the queue
     * 
     * @param tid (int) thread id [not used]
     * 
     * @note the `tid` parameter is not used but is kept for compatibility with the LinkedAdapter
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
};

template <typename T>
#ifdef DISABLE_PADDING
using BoundedSegmentCRQueue = BoundedSegmentAdapter<T, CRQueue<T, false>>;
template <typename T>
using BoundedItemCRQueue = BoundedItemAdapter<T, CRQueue<T, false>>;
template <typename T>
using LCRQueue = LinkedAdapter<T, CRQueue<T, false>>;
#else
using BoundedSegmentCRQueue = BoundedSegmentAdapter<T, CRQueue<T, true>>;
template <typename T>
using BoundedItemCRQueue = BoundedItemAdapter<T, CRQueue<T, true>>;
template <typename T>
using LCRQueue = LinkedAdapter<T, CRQueue<T, true>>;
#endif
