#pragma once

#include <atomic>
#include "LinkedRingQueue.hpp"
#include "RQCell.hpp"


template<typename T,bool padded_cells, bool bounded>
class PRQueue : public QueueSegmentBase<T, PRQueue<T,padded_cells,bounded>> {
private:
    using Base = QueueSegmentBase<T,PRQueue<T,padded_cells,bounded>>;
    using Cell = detail::CRQCell<void*,padded_cells>;
    
    Cell* array; 
    const size_t sizeRing;
#ifndef DISABLE_POW2
    const size_t mask;  //Mask to execute the modulo operation
#endif

    /**
     * @brief returns the index of the current node [without the MSB]
     * @param i (uint64_t) index
     * @return (uint64_t) index without the MSB
     */
    inline uint64_t nodeIndex(uint64_t i) const {
        return (i & ~(1ull << 63));
    }
    /**
     * @brief set the unsafe bit given an index
     * @param i (uint64_t) index
     * @return (uint64_t) index with the unsafe bit set
     */
    inline uint64_t setUnsafe(uint64_t i) const {return (i | (1ull << 63));}
    /**
     * @brief checks if a node is unsafe
     * @param i (uint64_t) index
     * @return (bool) true if the node is unsafe
     */
    inline bool nodeUnsafe(uint64_t i) const {
        return (i & (1ull << 63));
    }
    /**
     * @brief checks if a node is a bottom node
     * @param value (void*) value to check
     * @return (bool) true if the node is a bottom node
     */
    inline bool isBottom(void* const value) const {
        return (reinterpret_cast<uintptr_t>(value) & 1) != 0;
    }
    /**
     * @brief returns the bottom node for a given thread
     * @param tid (int) thread id
     * @return (void*) bottom node for the thread
     */
    inline void* threadLocalBottom(const int tid) const {
        return reinterpret_cast<void*>(static_cast<uintptr_t>((tid << 1) | 1));
    }

private:
    /**
     * @brief Private Constructor for PRQ segment
     * @param size_par (size_t) sizeRing of the segment
     * @param tid (int) thread id [not used]
     * @param start (uint64_t) start index of the segment [useful for LinkedRing Adaptation]
     * 
     * @note the `tid` parameters is not used but is kept for compatibility with the LinkedRingQueue
     * @note initializes the underlying segment array as well as the head and tail pointers
     */
    PRQueue(size_t size_par, [[maybe_unused]] const int tid, const uint64_t start): Base(),
#ifndef DISABLE_POW2
    sizeRing{detail::nextPowTwo(size_par)},
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
     * @brief Constructor for PRQ segment
     * @param size_par (size_t) sizeRing of the segment
     * @param tid (int) thread id [not used]
     * 
     * @note the `tid` parameters is not used but is kept for compatibility with the LinkedRingQueue
     * @note the segment is initialized with the start index set to 0
     */
    PRQueue(size_t size_par, [[maybe_unused]] const int tid = 0): PRQueue(size_par,tid,0){}

    /**
     * @brief Destructor for PRQ segment
     * 
     * Deletes the underlying array after draining it
     */
    ~PRQueue(){
        while(pop(0) != nullptr);
        delete[] array;
    }

    static std::string className(bool padding = true) {
        using namespace std::string_literals;
        return (bounded? "Bounded"s : ""s ) + "PRQueue"s + ((padded_cells && padding)? "/padded":"");
    }

    /***
     * @brief Push operation for PRQ
     * 
     * @param tid (int) thread id [not used]
     * 
     * @returns bool true if the operation succeeded else false
     * 
     * @note the `tid` parameter is not used but is kept for compatibility with the LinkedRingQueue
     * @note if the Segment is instantiates as bounded then the segment never closes
     * @warning uses 2CAS operation implemented as asm routines in x86Atomics.hpp
     */
    __attribute__((used,always_inline)) bool push(T* item,[[maybe_unused]] const int tid = 0) {
    
        while(true) {

            uint64_t tailTicket = Base::tail.fetch_add(1);
            
            if constexpr (bounded == false){
                if(Base::isClosed(tailTicket)) {
                    return false;
                }
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
                    if(cell.idx.compare_exchange_strong(idx,tailTicket + sizeRing)) {
                        if(cell.val.compare_exchange_strong(bottom, item)) {
                            return true;
                        }
                    } else {
                        cell.val.compare_exchange_strong(bottom, nullptr);
                    }
                }
            }

            if(tailTicket >= Base::head.load() + sizeRing){
                if constexpr (bounded){
                    return false;
                }
                else{
                    if (Base::closeSegment(tailTicket))
                        return false;
                }
            }  

        }
    }

    /***
     * @brief Pop operation for PRQ
     * 
     * @param tid (int) thread id [not used]
     * 
     * @returns (T*) item popped from the queue [nullptr if the queue is empty]
     * 
     * @note the `tid` parameter is not used but is kept for compatibility with the LinkedRingQueue
     * @warning uses 2CAS operation implemented as asm routines in x86Atomics.hpp
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
                    if(idx == headTicket + sizeRing){
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
                        if(cell.idx.compare_exchange_strong(cell_idx, unsafe | (headTicket + sizeRing)))
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

    /**
     * @brief returns the length of the queue
     * 
     * @note different implementation if the queue is bounded
     * @note otherwise it uses the QueueSegment underlying implementation
     */
    inline size_t length([[maybe_unused]] const int tid = 0) const {
        if constexpr (bounded){
            int length = Base::tail.load() - Base::head.load();
            if(length < 0) return 0;

            size_t _length = static_cast<size_t>(length);
            return _length > sizeRing ? sizeRing : _length;
        } else {
            return Base::length();
        }
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

public: 
    friend class LinkedRingQueue<T,PRQueue<T,padded_cells,bounded>>;   
  
};

/*
    Declare aliases for Unbounded and Bounded Queues
*/
template<typename T>
#ifndef DISABLE_PADDING
using LPRQueue = LinkedRingQueue<T,PRQueue<T,true,false>>;
template<typename T>
using BoundedPRQueue = PRQueue<T,true,true>;
#else 
using LPRQueue = LinkedRingQueue<T,PRQueue<T,false,false>>;
template <typename T>
using BoundedPRQueue = PRQueue<T,false,true>;
#endif
