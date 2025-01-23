#pragma once

#include <atomic>
#include "LinkedRingQueue.hpp"
#include "RQCell.hpp"
#include "x86Atomics.hpp"
#include "AdditionalWork.hpp"   //for loop function


#define BACKOFF_MIN 128UL //should be a power of 2
#define BACKOFF_MAX 1024UL //should be a power of 2

template <typename T,bool padded_cells, bool bounded>
class MTQueue : public QueueSegmentBase<T, MTQueue<T,padded_cells,bounded>>{
private:
    using Base = QueueSegmentBase<T, MTQueue<T,padded_cells,bounded>>;
    using Cell = detail::CRQCell<T*,padded_cells>;

    Cell *array;
    const size_t sizeRing;
#ifndef DISABLE_POW2
    const size_t mask;  //Mask to execute the modulo operation
#endif

private:
    /**
     * @brief Private Constructor for MTQueue segment
     * 
     * @param size_param (size_t) size of the segment
     * @param tid (int) thread id [not used]
     * @param start (uint64_t) start index of the segment [useful for LinkedRing Adaptation]
     * 
     * @note the `tid` parameters is not used but is kept for compatibility with the LinkedRingQueue
     * @exception std::invalid_argument if the size is 0
     */
    MTQueue(size_t size_param,[[maybe_unused]] const int tid, uint64_t start): 
    Base(),
#ifndef DISABLE_POW2
    sizeRing{detail::nextPowTwo(size_param)},
    mask{sizeRing - 1}
#else
    size{size_param}
#endif
    {
        if(sizeRing == 0)
            throw std::invalid_argument("Ring Size must be greater than 0");
        array = new Cell[sizeRing];
        for (uint64_t i = start; i < start + sizeRing; i++){
            array[i % sizeRing].val.store(nullptr,std::memory_order_relaxed);
            array[i % sizeRing].idx.store(i,std::memory_order_relaxed);

            Base::head.store(start,std::memory_order_relaxed);
            Base::tail.store(start,std::memory_order_relaxed);
        }
    }


public:
    /**
     * @brief Constructor for MTQueue segment
     * 
     * @param size (size_t) size of the segment
     * @param tid (int) thread id [not used]
     * 
     * @note the `tid` parameters is not used but is kept for compatibility with the LinkedRingQueue
     * @note calls the private constructor with start index set to 0
     */
    MTQueue(size_t size,[[maybe_unused]] const int tid = 0): MTQueue(size,tid,0){} 

    ~MTQueue(){
        while(pop(0) != nullptr);
        delete[] array;
    }

    static std::string className(bool padding = true) {
        using namespace std::string_literals;
        return (bounded? "Bounded"s : ""s ) + "MTQueue"s + ((padded_cells && padding)? "/padded":"");
    }

    /**
     * @brief Push operation for MTQueue
     * 
     * @param item (T*) item to push
     * @param tid (int) thread id [not used]
     * 
     * @returns (bool) true if the operation is successful
     * @note if queue is instantiated as bounded then the segment never closes
     * @note uses exponential decay backoff
     */
    __attribute__((used,always_inline)) bool push(T *item,[[maybe_unused]] const int tid){
        size_t tailTicket,idx;
        Cell *node;
        size_t bk = BACKOFF_MIN;
        while(true){
            tailTicket = Base::tail.load(std::memory_order_relaxed);
            if constexpr (!bounded){ //check if queue is closed if unbounded
                if(Base::isClosed(tailTicket)) {
                    return false;
                } 
            }
#ifndef DISABLE_POW2
            node = &(array[tailTicket & mask]);
#else
            node = &(array[tailTicket % size]);
#endif
            idx = node->idx.load(std::memory_order_acquire);
            if(tailTicket == idx){
                if(Base::tail.compare_exchange_strong(tailTicket,tailTicket + 1)) //try to advance the index
                    break;
                loop(bk);   //loop function that shoudnt be optimized out
                bk <<= 1;
                bk = ((bk-1) & (BACKOFF_MAX-1)) + 1;
            } else {
                if(tailTicket > idx){
                    if constexpr (bounded){ //if queue is bounded then never closes the segment
                        return false;
                    }
                    else{
                        /*
                            The Base::closeSegment function is designed to close segments for fetch_add queues
                            to compensate we use tailTicket - 1 to close the current segment
                        */
                        if (Base::closeSegment(tailTicket-1)){
                            return false;
                        }
                    }
                }
            }
        }
        node->val = item;
        node->idx.store((idx + 1),std::memory_order_release);
        return true;
    }

    /**
     * @brief Pop operation for MTQueue
     * 
     * @param tid (int) thread id [not used]
     * 
     * @returns (T*) item popped from the queue [nullptr if the queue is empty]
     * @note uses exponential decay backoff
     */
    __attribute__((used,always_inline)) T *pop([[maybe_unused]] const int tid){
        size_t headTicket,idx;
        Cell *node;
        size_t bk = BACKOFF_MIN;
        T* item;    //item to return;

        while(true){
            headTicket = Base::head.load(std::memory_order_relaxed);
#ifndef DISABLE_POW2
            node = &array[headTicket & mask];
#else
            node = &array[headTicket % size];
#endif
            idx = node->idx.load(std::memory_order_acquire);
            long diff = idx - (headTicket + 1);
            if(diff == 0){
                if(Base::head.compare_exchange_strong(headTicket,headTicket + 1)) //try to advance the head
                    break;
                loop (bk);  //loop function that shoudnt be optimized out
                bk <<= 1;
                bk = ((bk-1) & (BACKOFF_MAX-1)) + 1;;
            }
            else if (diff < 0){ //check if queue is empty
                if(Base::isEmpty())
                    return nullptr;
            }
        }

        item = node->val;
        node->idx.store(headTicket + sizeRing,std::memory_order_release);
        return item;
    }

    /**
     * @brief returns the length of the queue
     * 
     * @param tid (int) thread id [not used]
     * 
     * @returns (size_t) length of the queue [how many elements]
     * @note different implementation if the queue is bounded
     * @note otherwise it uses the QueueSegment underlying implementation
     * @warning the value returned could be an approximation.
     */
    inline size_t length([[maybe_unused]] const int tid = 0) const {
        if constexpr (bounded){
            int length = Base::tail.load() - Base::head.load();
            return length > 0 ? length : 0;
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


    friend class LinkedRingQueue<T,MTQueue<T,padded_cells,bounded>>;   

};

template<typename T>
#ifndef DISABLE_PADDING
using LMTQueue = LinkedRingQueue<T,MTQueue<T,true,false>>;
template<typename T>
using BoundedMTQueue = MTQueue<T,true,true>;
#else 
using LMTQueue = LinkedRingQueue<T,MTQueue<T,false,false>>;
template <typename T>
using BoundedMTQueue = MTQueue<T,false,true>;
#endif