#pragma once

// #include "LinkedAdapter.hpp"
// #include "BoundedSegmentAdapter.hpp"
// #include "BoundedItemAdapter.hpp"

#include "Segment.hpp"
#include "CacheRemap.hpp"
#include "RQCell.hpp"

// Forward declarations for wrapper classes
template <typename T, class Segment>
class LinkedAdapter;
template <typename T, class Segment>
class BoundedSegmentAdapter;
template <typename T, class Segment>
class BoundedItemAdapter;


#ifndef CACHE_LINE
#define CACHE_LINE 64ul
#endif

// max retries for closing segment [assuming spurious fails]
#ifndef TRY_CLOSE_PRQ
#define TRY_CLOSE_PRQ 10
#endif

template<typename T,bool padded_cells>
class PRQueue : public QueueSegmentBase<T, PRQueue<T,padded_cells>> {
private:

    using Base = QueueSegmentBase<T,PRQueue<T,padded_cells>>;
    using Cell = detail::CRQCell<void*,padded_cells>;

    const size_t sizeRing;
    const size_t mask;  //Mask for modulo as bitwise op

        /**
     * @brief Extracts the index from a given value [63 LSB]
     */
    inline uint64_t nodeIndex(uint64_t i) const {return (i & ~(1ull << 63));}

    /**
     * @brief Sets the MSB of a value to 1
     */
    inline uint64_t setUnsafe(uint64_t i) const {return (i | (1ull << 63));}

    /**
     * @brief Extracts the MSB of a value
     */
    inline uint64_t nodeUnsafe(uint64_t i) const {return (i & (1ull << 63));}

    /**
     * @brief Check if a given pointer is a bottom pointer
     * 
     * @note a bottom pointer is defined as a pointer with LSB set to 1
     */
    inline bool isReserved(void* const value) const { 
        return (reinterpret_cast<uintptr_t>(value) & 1) != 0; 
    }

    /**
     * @brief Computes a reserved per-thread value given a thread id
     * 
     * @param tid thread id
     * 
     * @warning correctness assumes that the thread id is less than 2^31
     * @warning correctness for LPRQ implementation the thread id must be unique
     */
    __attribute__((always_inline)) inline void * threadReserved(const int tid) const {
        return reinterpret_cast<void*>(static_cast<uintptr_t>((tid << 1) | 1));
    }

    Cell* array; 
    [[no_unique_address]] const CacheRemap<sizeof(Cell),CACHE_LINE> remap;

    //uses the tid argument to be consistent with linked queues
    PRQueue(size_t size_par, [[maybe_unused]] const int tid = 0, const uint64_t start = 0): Base(),
    sizeRing{size_par},
    mask(sizeRing - 1),
    remap(sizeRing) //initialize the cache remap
    {

#ifdef  DEBUG   //checks for the sizeRing to be a power of 2
#ifdef  DISABLE_POW2
        assert(sizeRing > 0);
#else   
        assert(sizeRing > 1 && detail::isPowTwo(sizeRing));
#endif 
#endif
        array = new Cell[sizeRing];

        for(uint64_t i = start; i < start + sizeRing; i++){
#ifdef DISABLE_POW2
            array[remap[i % sizeRing]].val.store(nullptr,std::memory_order_relaxed);
            array[remap[i % sizeRing]].idx.store(i,std::memory_order_relaxed);       
#else
            array[remap[i & mask]].val.store(nullptr,std::memory_order_relaxed);
            array[remap[i & mask]].idx.store(i,std::memory_order_relaxed);    
#endif 
        }

        Base::setStartIndex(start);
    }

public:
    ~PRQueue(){
        while(pop(0) != nullptr);
        delete[] array;
    }

    static std::string className(bool padding = true) {
        using namespace std::string_literals;
        return "PRQueue"s + ((padded_cells && padding)? "/padded":"");
    }

private:

    /**
     * @brief pushes an item on the queue
     * 
     * @param tid thread id
     * @param item a pointer to the item to be pushed
     * 
     * @note this operation is non-blocking
     * @warning assumes that thread ids are unique
     * 
     * @return true if the operation was successful, false otherwise
     * 
     * @warning if the operation is unsuccessful the operation shoudn't be called again [prone to consumer livelock]
     */
    __attribute__((used,always_inline)) bool push(T* item, const int tid) {
        int try_close = 0;

        while (true) {
            uint64_t tailticket = Base::tail.fetch_add(1);
            if (Base::isClosed(tailticket)) {
                return false;
            }

            void *bottom = threadReserved(tid);  //get the bottom pointer to perform the CAS operation

#ifdef DISABLE_POW2
            Cell& cell = array[remap[tailticket % sizeRing]];
#else
            Cell& cell = array[remap[tailticket & mask]];
#endif
            uint64_t idx = cell.idx.load();
            void* val   = cell.val.load();

            if (val == nullptr){
                if(nodeIndex(idx) <= tailticket){
                    if((!nodeUnsafe(idx) || Base::head.load() <= tailticket)) {
                        if (cell.val.compare_exchange_strong(val, bottom)) {
                            if (cell.idx.compare_exchange_strong(idx, tailticket + sizeRing)) {
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
            if (tailticket >= Base::head.load() + sizeRing) {
                if (Base::closeSegment(tailticket, ++try_close > TRY_CLOSE_PRQ))
                    return false;
            }
        }
    }

    /**
     * @brief pushes an item on the queue
     * 
     * @param tid thread id [not used - keeps same interface]
     * @param item a pointer to the item to be pushed
     * 
     * @note this operation is non-blocking
     * 
     * @return true if the operation was successful, false otherwise [empty queue]
     */
    __attribute__((used,always_inline)) T* pop([[maybe_unused]] const int tid = 0) {
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
            int r       = 0;
            uint64_t tt = 0;

            while (true) {
                uint64_t cell_idx   = cell.idx.load();
                uint64_t unsafe     = Base::getMSB(cell_idx);
                uint64_t idx        = Base::get63LSB(cell_idx);
                void* val           = cell.val.load();

                //inconsistent view of the cell
                if(cell_idx != cell.idx.load())
                    continue;

                if (idx > (headticket + sizeRing))
                    break;

                if ((val != nullptr) && !isReserved(val)) {
                    if (idx == (headticket + sizeRing)) {   //only one dequeuer will pass this condition
                        cell.val.store(nullptr);
                        return static_cast<T*>(val);
                    } else {
                        if (unsafe) {
                            if (cell.idx.load() == cell_idx)
                                break;
                        } else {
                            if (cell.idx.compare_exchange_strong(cell_idx, Base::setMSB(idx)))
                                break;
                        }
                    }
                } else {

                    //every tot iterations loads the tail pointer [limits invalidation rate]
                    if ((r & ((1ull << 8) - 1)) == 0)
                        tt = Base::tail.load();

                    uint64_t t = Base::tailIndex(tt);
                    /**
                     * Conditions: 
                     * unsafe cell (dequeuers are not dequeuing | could be a bottom pointer)
                     * the tail is behind (high contention)
                     * the queue is closed
                     * thread spinning for too long
                     */
                    if (unsafe || t < (headticket + 1) || Base::isClosed(tt) || (r > (1 << 12))) {
                        //if the value is a bottom pointer, that needs to be cleared (try to clean) and check for enqueue
                        if (isReserved(val) && !cell.val.compare_exchange_strong(val, nullptr))
                            continue;
                        //if the cell is unsafe then it's shifted to the next epoch (another dequeuer dequeue will try to fix it)
                        if (cell.idx.compare_exchange_strong(cell_idx, unsafe | (headticket + sizeRing))){
                            break;
                        }
                    }
                    ++r;    //increment the retry counter
                }
            }

            /**
             * check if the queue is empty
             * 
             * @note if current segment is closed then the condition is always true
             */
            if (Base::tailIndex() <= headticket + 1) {
                Base::fixState();
                return nullptr;
            }
        }
    
    }

    /**
     * @note computes the length [element count] currently in the queue
     * 
     * @warning this value is not accurate and should be used only for debug purposes
     */
    inline size_t length([[maybe_unused]] const int tid = 0) const {
        return Base::length();
    }
    

    friend class LinkedAdapter<T,PRQueue<T,padded_cells>>;   
    friend class BoundedItemAdapter<T,PRQueue<T,padded_cells>>;
    friend class BoundedSegmentAdapter<T,PRQueue<T,padded_cells>>;
  
};

// Aliases
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