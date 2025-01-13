#pragma once
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstddef>
#include "RQCell.hpp"
#include "HazardPointers.hpp"

#ifndef CACHE_LINE
#define CACHE_LINE 64
#endif

template<typename T, bool padded_cells>
class FAAArrayQueue {
private:

    struct Node;
    using Cell = detail::PlainCell<T*,padded_cells>;
    const size_t sizeRing;
    const size_t maxThreads;
    HazardPointers<Node> HP;
    const int kHpTail = 0;
    const int kHpHead = 1;

    alignas(CACHE_LINE) std::atomic<Node*> head;
    alignas(CACHE_LINE) std::atomic<Node*> tail;
    T* taken = (T*)new int(); //alloca un puntatore a intero e lo casta a T

    /**
     * @brief Node structure for the FAAArrayQueue
     * 
     * @param deqidx (std::atomic<int>) index for the dequeue operation
     * @param enqidx (std::atomic<int>) index for the enqueue operation
     * @param next (std::atomic<Node*>) pointer to the next node
     * @param items (Cell*) array of cells
     * 
     * @note the `alignas(CACHE_LINE)` directive is used to align the structure to the cache line sizeRing
     * @note the `startIndexOffset` is used to calculate the index of the first element in the array
     */
    struct Node {
        alignas(CACHE_LINE) std::atomic<size_t>    deqidx;
        alignas(CACHE_LINE) std::atomic<size_t>    enqidx;
        alignas(CACHE_LINE) std::atomic<Node*>  next;
        Cell *items;
        const uint64_t startIndexOffset;

        //First entry prefilled [Sentinel]
        Node(T* item, uint64_t startIndexOffset,size_t Buffer_Size=128):
        deqidx{0}, enqidx{1},
        next{nullptr}, startIndexOffset(startIndexOffset)
        {
            items = new Cell[Buffer_Size];
            items[0].val.store(item,std::memory_order_relaxed);
            for(size_t i = 1; i < Buffer_Size; i++){
                items[i].val.store(nullptr,std::memory_order_relaxed);
            }
        }

        ~Node(){
            delete[] items;
        }

        /**
         * @brief Compare and Swap for the next pointer
         * @param cmp (Node*) pointer to compare
         * @param val (Node*) pointer to set
         * @return (bool) true if the operation is successful
         */
        inline bool casNext(Node *cmp, Node *val) {
            return next.compare_exchange_strong(cmp,val);
        }
    };

    /**
     * @brief Compare and Swap for the tail pointer
     */
    inline bool casTail(Node *cmp, Node *val) {
        return tail.compare_exchange_strong(cmp,val);
    }

    /**
     * @brief Compare and Swap for the head pointer
     */
    inline bool casHead(Node *cmp, Node *val) {
        return head.compare_exchange_strong(cmp,val);
    }

public:
    /**
     * @brief Constructor for the FAAArrayQueue
     * @param Buffer_Size (size_t) sizeRing of the buffer
     * @param maxThreads (size_t) number of threads
     * 
     * The constructor initializes the head and tail pointers to a new node
     * @note the `maxThreads` parameter is used to initialize the HazardPointers matrix
     * @note assert that the buffer sizeRing is greater than 0
     */
    FAAArrayQueue(size_t Buffer_Size, size_t maxThreads):
    sizeRing{Buffer_Size},maxThreads{maxThreads},
    HP(2,maxThreads)
    {
        assert(Buffer_Size > 0);
        Node* sentinelNode = new Node(nullptr,0,Buffer_Size);
        sentinelNode->enqidx.store(0,std::memory_order_relaxed);
        head.store(sentinelNode, std::memory_order_relaxed);
        tail.store(sentinelNode, std::memory_order_relaxed);
    }

    /**
     * @brief Destructor for the FAAArrayQueue
     * 
     * Deletes all nodes in the queue (also drains the queue)
     */
    ~FAAArrayQueue() {
        while(pop(0) != nullptr);
        delete head.load();
        delete (int*)taken;
    }

    static std::string className(bool padding = true) {
        using namespace std::string_literals;
        return "FAAArrayQueue"s + ((padded_cells && padding)? "/padded" : "");
    }

    /**
     * @brief Returns the current sizeRing of the queue
     * @param tid (int) thread id
     * 
     * @return (size_t) sizeRing of the queue
     * 
     * @note this operation is non-blocking
     * @note the value returned could be an approximation. Depends on the Segment implementation
     */
    size_t length(int tid) {
        Node* lhead = HP.protect(kHpHead,head,tid);
        Node* ltail = HP.protect(kHpTail,tail,tid);

        uint64_t t = std::min((uint64_t) sizeRing, ((uint64_t) ltail->enqidx.load())) + ltail->startIndexOffset;
        uint64_t h = std::min((uint64_t) sizeRing, ((uint64_t) lhead->deqidx.load())) + lhead->startIndexOffset;
        HP.clear(tid);
        return t > h ? t - h : 0;
    }

    /**
     * @brief Push operation for the FAAArrayQueue
     * @param item (T*) item to push
     * @param tid (int) thread id
     * 
     * @note this operation is non-blocking and always succeeds
     * @note the `tid` parameter is used to protect the head and tail pointers via HazardPointers matrix
     * @exception std::invalid_argument if the item is a null pointer
     */
    __attribute__((used,always_inline)) void push(T* item, const int tid) {
        if(item == nullptr)
            throw std::invalid_argument("item cannot be null pointer");

        while(1){
            Node *ltail = HP.protect(kHpTail,tail,tid);
            const size_t idx = ltail->enqidx.fetch_add(1);
            if(idx >(sizeRing-1)) { 
                if(ltail != tail.load()) continue;
                Node* lnext = ltail->next.load(); 
                if(lnext == nullptr) {
                    Node* newNode = new Node(item,ltail->startIndexOffset + sizeRing, sizeRing);
                    if(ltail->casNext(nullptr,newNode)) {
                        casTail(ltail, newNode);
                        HP.clear(kHpTail,tid);
                        return;
                    }
                    delete newNode;
                } else {
                    casTail(ltail,lnext);
                }
                continue;
            }

            T* itemNull = nullptr;
            if(ltail->items[idx].val.compare_exchange_strong(itemNull,item)) {
                HP.clear(kHpTail,tid);
                return;
            }
        }
    }

    /**
     * @brief Pop operation for the FAAArrayQueue
     * @param tid (int) thread id
     * 
     * @returns (T*) item popped from the queue [nullptr if the queue is empty]
     * 
     * @note this operation is non-blocking
     * @note the `tid` parameter is used to protect the head and tail pointers via HazardPointers matrix
     */
    __attribute__((used,always_inline)) T* pop(const int tid) {
        T* item = nullptr;
        Node* lhead = HP.protect(kHpHead, head, tid);

#ifdef CAUTIOUS_DEQUEUE
        if (lhead->deqidx.load() >= lhead->enqidx.load() && lhead->next.load() == nullptr)
            return nullptr;
#endif

        while (true) {
            const size_t idx = lhead->deqidx.fetch_add(1);
            if (idx > sizeRing-1) { // This node has been drained, check if there is another one
                Node* lnext = lhead->next.load();
                if (lnext == nullptr) {
                    break;  // No more nodes in the queue
                }
                if (casHead(lhead, lnext))
                    HP.retire(lhead, tid);

                lhead = HP.protect(kHpHead, head, tid);
                continue;
            }
            Cell& cell = lhead->items[idx];
            if (cell.val.load() == nullptr && idx < lhead->enqidx.load()) {
                for (size_t i = 0; i < 4*1024; ++i) {
                    if (cell.val.load() != nullptr)
                        break;
                }
            }
            item = cell.val.exchange(taken);
            if (item != nullptr)
                break;

            size_t t = lhead->enqidx.load();
            if (idx + 1 >= t) {
                if (lhead->next.load() != nullptr)
                    continue;
                lhead->enqidx.compare_exchange_strong(t, idx + 1);
                break;
            }
        }
        HP.clear(kHpHead, tid);
        return item;
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

// Type Aliases for ease of use
#ifndef DISABLE_PADDING
template<typename T>
using FAAQueue = FAAArrayQueue<T,true>;
#else
template<typename T>
using FAAQueue = FAAArrayQueue<T,false>;
#endif
