#include <vector>
#include <memory>
#include "SPSC.hpp"

/**
 * All2All emulates a MPMC queue using a network of SPSC queues (one for each producer-consumer pair)
 * 
 * every thread accesses its queue using it's tid argument
 * 
 * @warning this supposes that each thread has a unique id
 */
template<typename T>
class All2All {
private:
    std::vector<std::vector<std::unique_ptr<SPSC<T>>>> queueMatrix;
    const size_t producers;
    const size_t consumers;
    /**
     * Producers and consumers start from the index 0
     * If a queue is full they switch to the next one
     * 
     * The switch happens only on failed push or failed pop
     * in order to avoid CACHE_trash
     */
    static inline size_t producer_idx = 0;    //start sending to consumer 0
    static inline size_t consumer_idx = 0;    //start receiving from consumer 0

public:
    All2All(size_t sizeQueue,size_t producers_par, size_t consumers_par):
    producers{producers_par},
    consumers{consumers_par}
    {
        if(producers == 0 || consumers == 0)
            throw std::invalid_argument("Producers and/or Consumers node must be greater than 0");

        if(sizeQueue == 0)
            throw std::invalid_argument("Size of underlying queues must be greater than 0");

       //initialize the queue matrix
       queueMatrix.reserve(producers);
       for(size_t i = 0; i < producers; i++){
            queueMatrix.push_back(std::vector<std::unique_ptr<SPSC<T>>>());
            for(size_t j = 0; j < consumers; j++){
                queueMatrix[i].emplace_back(std::make_unique<SPSC<T>>(sizeQueue));
            }
       }
    }

    ~All2All(){};

    //parameter to keep the method compatible
    static std::string className(bool padding = false){
        return "All2All";
    }

    /**
     * We keep track of the current producer index and we try to push from that queue, if push is successful
     * then return the item, if not, we circle around the queue updating the 
     * 
     * @warning if multiple producer invoke the method with the same tid index then undefined behaviour (Most likely breaks FIFO semantics)
     * 
     * @note uses 2 forLoops to avoid modulo arithmetic
     */
    __attribute__((used,always_inline)) bool inline push(T* item, const int tid){
        const size_t producer = tid % producers;    //get the producer queueSet
        size_t cons = producer_idx; //get the current consumer index

        //try push on current consumer
        if((queueMatrix[producer][cons])->push(item))
            return true;

        //fallback try other consumers [right]
        for(size_t i = cons+1; i < consumers; i++){
            if((queueMatrix[producer][i])->push(item)){
                producer_idx = i;  //change the current index only on successful push
                return true;
            }
        }
        //fallback try other consumers [left]
        for(size_t i = 0; i < cons; i++){
            if((queueMatrix[producer][i])->push(item)){
                producer_idx = i;  //change the current index only on successful push
                return true;
            }
        }
        
        return false;
    }

    /**
     * We keep track of the current producer index and we try to push from that queue, if push is successful
     * then return the item, if not, we circle around the queue updating the 
     * 
     * @warning if multiple producer invoke the method with the same tid index then undefined behaviour (Most likely breaks FIFO semantics)
     */
    __attribute__((used,always_inline)) inline T* pop(const int tid){
        const size_t consumer = tid % consumers;    //get the current consumer
        size_t prod = producer_idx; //get the consumer queueSet

        //try pop on current consumer
        T* item = (queueMatrix[prod][consumer])->pop();
        if(item != nullptr) return item;

        //fallback try other producers [right]
        for(size_t i = prod+1; i < producers; i++){
            item = (queueMatrix[i][consumer])->pop();
            if(item != nullptr){
                consumer_idx = i;  //change the current index only on successful pop
                return item;
            }
        }

        //fallback try other consumers [left] 
        for(size_t i = 0; i < prod; i++){
            item = (queueMatrix[i][consumer])->pop();
            if(item != nullptr){
                consumer_idx = i;  //change the current index only on successful pop
                return item;
            }
        }

        return nullptr;

    }
};