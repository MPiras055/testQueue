#pragma once

// #include "LinkedAdapter.hpp"
// #include "BoundedSegmentAdapter.hpp"
// #include "BoundedItemAdapter.hpp"

#include "Segment.hpp"      //for QueueSegmentBase
#include "CacheRemap.hpp"
#include "RQCell.hpp"
#include "x86Atomics.hpp"   //for CAS2

// Forward declarations for wrapper classes
template <typename T, class Segment>
class LinkedAdapter;
template <typename T, class Segment>
class BoundedSegmentAdapter;
template <typename T, class Segment>
class BoundedItemAdapter;



//max retries for closing segment [assuming spurious fails]
#ifndef TRY_CLOSE_CRQ 
#define TRY_CLOSE_CRQ 10
#endif

#ifndef CACHE_LINE
#define CACHE_LINE 64ul
#endif

template <typename T, bool padded_cells>
class CRQueue : public QueueSegmentBase<T, CRQueue<T, padded_cells>> {
private:

    using Base = QueueSegmentBase<T, CRQueue<T, padded_cells>>;
    using Cell = detail::CRQCell<T *, padded_cells>;

    const size_t sizeRing;
    const size_t mask;  //Mask for modulo as bitwise op

    [[no_unique_address]] const CacheRemap<sizeof(Cell), CACHE_LINE> remap;
    Cell *array;

    /**
     * @brief Private Constructor for CRQ segment
     * 
     * @param size_par (size_t) sizeRing of the segment
     * @param tid (int) thread id [not used]
     * @param start (uint64_t) start index of the segment [useful for LinkedRing Adapter]
     * 
     * @note the `tid` parameters is not used but is kept for compatibility with the LPRQ
     */
    CRQueue(size_t size_par,[[maybe_unused]] const int tid = 0, const uint64_t start = 0): Base(), 
    sizeRing{size_par},
    mask{sizeRing - 1},
    remap(sizeRing) //initialize the cache remap
    {

#ifdef  DEBUG
#ifdef  DISABLE_POW2
        assert(sizeRing > 0);
#else   
        assert(sizeRing > 1 && detail::isPowTwo(sizeRing));
#endif 
#endif
        array = new Cell[sizeRing];

        for (uint64_t i = start; i < start + sizeRing; i++) {
#ifdef DISABLE_POW2
            array[remap[i % sizeRing]].val.store(nullptr, std::memory_order_relaxed);
            array[remap[i % sizeRing]].idx.store(i, std::memory_order_relaxed);
#else
            array[remap[i & mask]].val.store(nullptr, std::memory_order_relaxed);
            array[remap[i & mask]].idx.store(i, std::memory_order_relaxed);
#endif
        }

        Base::setStartIndex(start);
    }

    /**
     * @brief Destructor for CRQ segment
     * 
     * Deletes the underlying array of the segment after draining it
     */
public:

    ~CRQueue() {
        while(pop(0) != nullptr);
        delete[] array; 
    }

    static std::string className(bool padding = true){
        using namespace std::string_literals;
        return "CRQueue"s + ((padded_cells && padding)? "/padded":"");
    }

private:
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
                if (Base::get63LSB(idx) <= tailticket) {
                    if ((!Base::getMSB(idx) || Base::head.load() < tailticket)) {
                        if (CAS2((void**)&cell, nullptr, idx, item, tailticket)) {
                            return true;
                        }
                    }
                }
            }

            if (tailticket >= Base::head.load() + sizeRing) {
                if (Base::closeSegment(tailticket, ++try_close > TRY_CLOSE_CRQ))
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
                uint64_t unsafe = Base::getMSB(cell_idx);
                uint64_t idx = Base::get63LSB(cell_idx);
                T* val = static_cast<T*>(cell.val.load());

                if (idx > headticket)
                    break;

                if (val != nullptr) {   //Dequeue transition
                    if (idx == headticket) {
                        if (CAS2((void**)&cell, val, cell_idx, nullptr, unsafe | (headticket + sizeRing))) {
                            return val;
                        }
                    } else {            //Unsafe transition
                        if (CAS2((void**)&cell, val, cell_idx, val, Base::setMSB(idx)))
                            break;
                    }
                } else {
                    if ((r & ((1ull << 8) - 1)) == 0)
                        tt = Base::tail.load();

                    uint64_t t = Base::tailIndex(tt);
                    if (unsafe || t < headticket + 1 || Base::isClosed(tt) || r > (1 << 12)) {
                        if (CAS2((void**)&cell, val, cell_idx, val, unsafe | (headticket + sizeRing)))
                            break;
                    }
                    ++r;
                }
            }

            if (Base::tailIndex() <= (headticket + 1)) {
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

    friend class LinkedAdapter<T,CRQueue<T,padded_cells>>;   
    friend class BoundedItemAdapter<T,CRQueue<T,padded_cells>>;
    friend class BoundedSegmentAdapter<T,CRQueue<T,padded_cells>>;

};


//Aliases
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
