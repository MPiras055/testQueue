#pragma once

#include "RQCell.hpp"
#include "CacheRemap.hpp"
#include "Segment.hpp"

//Forward declaration for LinkedrAdapter
template<typename T, class Segment>
class LinkedAdapter; 

#ifndef TRY_CLOSE_MTQ
#define TRY_CLOSE_MTQ 10
#endif

//delay for CAS retry
#define __MTQ_MIN_DELAY 128ul
#define __MTQ_MAX_DELAY 1024ul


/**
 * follows the implementation of a MPMC queue segment based on CAS-loop
 * 
 * @note atomic ops are in respect to std::memory_order_acquire - release
 */
template <typename T,bool padded_cells, bool bounded>
class MTQueue : public QueueSegmentBase<T, MTQueue<T,padded_cells,bounded>>{
private:
    using Base = QueueSegmentBase<T, MTQueue<T,padded_cells,bounded>>;
    using Cell = detail::CRQCell<T*,padded_cells>;

    Cell *array;
    const size_t sizeRing;
    const size_t mask;
    [[no_unique_address]] const CacheRemap<sizeof(Cell), CACHE_LINE> remap;


public:
    /**
     * @brief Private Constructor for MTQueue segment
     * 
     * @param size_param (size_t) size of the segment
     * @param tid (int) thread id [not used]
     * @param start (uint64_t) start index of the segment [useful for LinkedRing Adaptation]
     * 
     * @note the `tid` parameters is not used but is kept for compatibility with the LinkedAdapter
     * 
     * @details checks DISABLE_POW2 macro for the bounded version
     */
    MTQueue(size_t size_par,[[maybe_unused]] const int maxThreads = 0, uint64_t start = 0): Base(), 
#ifndef DISABLE_POW2
    sizeRing{ size_par > 1 && detail::isPowTwo(size_par) ? size_par : detail::nextPowTwo(size_par)}, //round up to next power of 2
#else
    sizeRing{size_par},
#endif
    mask{sizeRing - 1},
    remap(sizeRing) //initialize the cache remap
    {
#ifdef DEBUG
#ifdef DISABLE_POW2
        assert(sizeRing > 0);
#else
        assert(sizeRing > 1 && detail::isPowTwo(sizeRing));
#endif
#endif

        array = new Cell[sizeRing];

        //sets the fields of the Cell given the startingIndex (default 0)
        for(uint64_t i = start; i < start + sizeRing; ++i){
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

    /**
     * @brief Destructor
     * 
     * Drains the segment and deallocates the underlying array
     */
    ~MTQueue(){
        while(pop(0) != nullptr);
        delete[] array;
    }

    static std::string className(bool padding = true) {
        using namespace std::string_literals;
        return (bounded? "Bounded"s : ""s ) + "MTQueue"s + ((padding && padded_cells)? "/padded":"");
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
        unsigned long delay = __MTQ_MIN_DELAY;

        int try_close = 0;
        size_t tailTicket,idx;
        Cell *node;
        while(true){
            tailTicket = Base::tail.load(std::memory_order_relaxed);

            if constexpr (!bounded){ //check if queue is closed if unbounded
                if(Base::isClosed(tailTicket)) {
                    return false;
                } 
            }

#ifndef DISABLE_POW2
            node = &(array[remap[tailTicket & mask]]);
#else
            node = &(array[remap[tailTicket % sizeRing]]);
#endif
            idx = node->idx.load(std::memory_order_acquire);
            if(tailTicket == idx){
                if(Base::tail.compare_exchange_weak(tailTicket,tailTicket + 1, std::memory_order_relaxed)) //try to advance the index
                    break;

                delay = backoff(delay); //delay for CAS retry

            } else if(tailTicket > idx){
                if constexpr (!bounded){ //if queue is bounded then never closes the segment
                    if(Base::closeSegment(tailTicket - 1, ++try_close > TRY_CLOSE_MTQ))
                        return false;
                }
                else{
                    return false;
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
    __attribute__((used,always_inline)) T *pop ([[maybe_unused]] const int tid){
        unsigned long delay = __MTQ_MIN_DELAY;
        size_t headTicket, idx;
        Cell *node;
        T* item;

        while(true){
            headTicket = Base::head.load(std::memory_order_relaxed);

#ifndef DISABLE_POW2
            node = &(array[remap[headTicket & mask]]);
#else
            node = &(array[remap[headTicket % sizeRing]]);
#endif

            idx = node->idx.load(std::memory_order_acquire);
            long diff = idx - (headTicket + 1);
            if(diff == 0){
                if(Base::head.compare_exchange_weak(headTicket,headTicket + 1,std::memory_order_relaxed)) //try to advance the head
                    break;
                delay = backoff(delay); //delay for CAS retry
                
            }
            else if (diff < 0){
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

private:
    /**
     * Private method to perform between-CAS delay
     */
    __attribute__((used,always_inline)) unsigned long backoff(unsigned long currentDelay){
#ifdef DEBUG
        assert(currentDelay >= __MTQ_MIN_DELAY && currentDelay <= __MTQ_MAX_DELAY); //in bounds check
#endif
        //Wait the current delay
        while((currentDelay--) != 0)
            asm volatile("nop");    //busy waiting iterations
        
        //update the delay for next iteration
        currentDelay = currentDelay << 1;
        return currentDelay > __MTQ_MAX_DELAY ? __MTQ_MAX_DELAY : currentDelay;
    }


    friend class LinkedAdapter<T,MTQueue<T,padded_cells,bounded>>;   

};

template<typename T>
#ifndef DISABLE_PADDING
using LMTQueue = LinkedAdapter<T,MTQueue<T,true,false>>;
template<typename T>
using BoundedMTQueue = MTQueue<T,true,true>;
#else 
using LMTQueue = LinkedAdapter<T,MTQueue<T,false,false>>;
template <typename T>
using BoundedMTQueue = MTQueue<T,false,true>;
#endif