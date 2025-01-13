#pragma once
#include <cstddef>  // For alignas
#include <mutex>
#include <deque>
#include <atomic>
#include <string>


template<typename T, bool bounded>
class MuxQueue {
private:
    std::deque<T*> queue;
    std::mutex       mux;   
    size_t           sizeRing;


public:
    /**
     * @brief Constructor for MuxQueue
     * 
     * @param size_par (size_t) sizeRing of the queue
     * @param tid (int) thread id [not used]
     * 
     * @note the `tid` parameter is not used but is kept for compatibility with the LinkedRingQueue
     * @note uses a std::deque as the underlying data structure
     */
    MuxQueue(size_t size_par, [[maybe_unused]] const int tid = 0):sizeRing{size_par}{};
    ~MuxQueue(){};

    /**
     * @brief Returns the current sizeRing of the queue
     * @param tid (int) thread id
     * 
     * @return (size_t) sizeRing of the queue
     * 
     * @note this operation is non-blocking
     * @note the value returned could be an approximation. Depends on the Segment implementation
     * @note this function is thread-safe as it uses a std::lock_guard to ensure mutual exclusion
     */
    size_t length([[maybe_unused]] const int tid = 0){ 
        std::lock_guard<std::mutex> lock(mux);
        return queue.size();
    }

    static inline std::string className([[maybe_unused]] bool padding=true){
        using namespace std::string_literals;
        return (bounded? "Bounded"s : "Linked"s) + "MuxQueue"s;
    }

    /**
     * @brief Push operation for MuxQueue
     * 
     * @param item (T*) item to push
     * @param tid (int) thread id [not used]
     * 
     * @returns (bool) true if the operation is successful
     * 
     * @note the `tid` parameter is not used but is kept for compatibility with the LinkedRingQueue
     * @note uses a std::lock_guard to ensure mutual exclusion
     */
    __attribute__((used,always_inline)) bool push(T* item,[[maybe_unused]] const int tid = 0){
        std::lock_guard<std::mutex> lock(mux);
        if constexpr (bounded){
            if(queue.size() >= sizeRing) return false;
        }
        queue.push_back(item);
        return true;
    }

    /** 
     * @brief Pop operation for MuxQueue
     * 
     * @param tid (int) thread id [not used]
     * 
     * @returns (T*) item popped from the queue [nullptr if the queue is empty]
     * 
     * @note the `tid` parameter is not used but is kept for compatibility with the LinkedRingQueue
     * @note uses a std::lock_guard to ensure mutual exclusion
     */
    __attribute__((used,always_inline)) T* pop([[maybe_unused]] const int tid = 0){
        T* item;
        std::lock_guard<std::mutex> lock(mux);
        if(queue.empty()) return nullptr;
        item = queue.front();
        queue.pop_front();
        return item;
    }

    /**
     * @brief returns the size of the queue
     * 
     * @returns (size_t) size of the queue
     * @note added for compatibility in testing with other queues
     */
    inline size_t capacity() const {
        return sizeRing;
    }

};

template<typename T>
using BoundedMuxQueue = MuxQueue<T,true>;
template<typename T>
using LinkedMuxQueue = MuxQueue<T,false>;