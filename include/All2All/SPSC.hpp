#pragma once
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <RQCell.hpp>

/**
 * Multipush allows a thread to push to a local buffer then to push to the global queue
 */

//multipush doesn't work yet
#undef MULTIPUSH

#ifdef MULTIPUSH
#ifndef MPUSH_BUFFER
#define MPUSH_BUFFER 16
#endif
#endif

#ifndef CACHE_LINE
#define CACHE_LINE 64
#endif

template <typename T, bool padded>
class SPSCQueue {
private:

    alignas(CACHE_LINE) std::atomic<uint64_t> head;
    alignas(CACHE_LINE) std::atomic<uint64_t> tail;
    void **buffer;
    const uint64_t size;
#ifdef MULTIPUSH
    void *local_buffer[MPUSH_BUFFER];
        uint64_t mpush_length;
    const uint64_t mpush_size;
#endif

public:
    SPSCQueue(uint64_t size_par) :
#ifdef MULTIPUSH
    mpush_length(0),
    mpush_size(MPUSH_BUFFER),
#endif
    size{size_par}
    {
#ifdef MULTIPUSH
        if(size < MPUSH_BUFFER) {
            throw std::invalid_argument("Size must be equal or greater than MPUSH_BUFFER");
        }
#endif

        //for aligned_alloc to work the size must be a power of 2 greater equal to the CACHE_LINE
        assert(size >= CACHE_LINE);
        assert(detail::isPowTwo(size));

        buffer = (void **) aligned_alloc(CACHE_LINE * sizeof(void *),size * sizeof(void *));
        if(!buffer)
            throw std::bad_alloc(); 

        //init the buffer;
        std::memset(buffer,0,sizeof(void *)*size);    //set the buffer to null;
        

    }

    ~SPSCQueue() {
        if(buffer != nullptr) {
            std::free(buffer);  //use free because allocating with aligned_alloc
        }
    }

    __attribute__((always_inline,used)) inline bool available() {
        const void *curr_tail = buffer[tail];
        return curr_tail == nullptr;
    }

    __attribute__((always_inline,used)) inline bool empty() {
        const void *curr_head = buffer[head];
        return curr_head == nullptr;
    }
#ifdef MULTIPUSH
    __attribute__((always_inline,used)) inline bool flush() {
        if(mpush_length == 0) return true;
        else return multipush();
    }
#endif

    /***
     * @brief pushes an item into the queue
     * 
     * @param item 
     * 
     * @note if multipush defined then the item can experience local buffering
     * 
     * @warning `MULTIPUSH` the method can be successful if the item is pushed in the local buffer
     */
    __attribute__((always_inline,used)) inline bool push(T *item) {
        assert(item != nullptr);
#ifdef MULTIPUSH
        //flush the local buffer if full
        if((mpush_length >= mpush_size) && !flush()) return false;
        local_buffer[mpush_length++] = reinterpret_cast<void *>(item);
        if(mpush_length >= mpush_size) flush();
        return true;
#else
        if(available()) {
            buffer[tail] = reinterpret_cast<void *>(item);
            std::atomic_thread_fence(std::memory_order_release);   //all pending 
            tail = tail + (tail + 1 == size ? 1 - size : 1);
            return true;
        }
        return false;
#endif
    } 

    __attribute__((always_inline,used)) inline T *pop() {
        if(empty()) return nullptr;
        T *item = reinterpret_cast<T*>(buffer[head]);
        buffer[head] = nullptr;
        head = head + (head + 1 == size ? 1 - size : 1);
        return item;

    }
#ifdef MULTIPUSH
    __attribute__((always_inline,used)) inline bool multipush() {
        assert(mpush_length <= mpush_size); //check for buffer overflow
        assert(mpush_length <= size); //multipush max size should not exceed the queue size
        uint len = mpush_length;
        uint last = tail + ((tail + --len >= size) ? (len - size) : len );
        uint right = len - (last + 1);
        uint left = last;
        uint i;

        if(buffer[last].load(std::memory_order_relaxed) != nullptr) return false;

        if(last < tail) {   // queue has revolved
            for(i = len; i > right; --i, --left) {
                buffer[left].store(local_buffer[i], std::memory_order_release);
            }
            for(i = size - 1 ; i >= tail ; --i, --right) {
                buffer[i].store(local_buffer[right], std::memory_order_release);
            }
            puts("PUSHED LEFT");
        } else {
            for(int i = len; i >= 0; --i){
                buffer[i].store(local_buffer[i], std::memory_order_release);
            }
            puts("PUSHED RIGHT");
        }

        tail = tail + ((last + 1 >= size) ? 0 : (last + 1));
        mpush_length = 0;

        return true;
    }
#endif
};

template <typename T>
#ifndef DISABLE_PADDING
using SPSC = SPSCQueue<T, true>;
#else
using SPSC = SPSCQueue<T, false>;
#endif
