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
    std::vector<std::vector<std::unique_ptr<SPSC<T>>>> queueMatrix; //matrix[iProducer][jConsumer]
    const size_t producers;
    const size_t consumers;
    
    /**
     * each thread mantains a thread_local variables (if the thread does not act as producer & consumer only
     * one is used) to keep track of the current index where to attempt the next push/pop
     */
    static inline thread_local size_t current_producer = 0; //used by consumers
    static inline thread_local size_t current_consumer = 0; //used by producers

public:
    All2All(size_t size,size_t producers_par, size_t consumers_par):
    producers{producers_par},
    consumers{consumers_par}
    {
        if(producers == 0 || consumers == 0)
            throw std::invalid_argument("Producers and/or Consumers node must be greater than 0");

        if(size == 0)
            throw std::invalid_argument("Size of queues must be greater than 0");

        //split the size of the queue in equal parts
        const size_t sizeQueue = size / (producers * consumers);
        if(sizeQueue == 0)
            throw std::invalid_argument("Size of underlying queue too low | need at least " + producers * consumers);
        
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
     * then return the item, if not, we circle around the queue updating the producer_index variable
     * 
     * @note RR scheduling: if the push is successful next push will try from next queue. If failed, circle around the queue
     * 
     * @warning if multiple producer invoke the method with the same tid index then undefined behaviour (Most likely breaks FIFO semantics)
     * 
     * 
     */
    __attribute__((used,always_inline)) bool inline push(T* item, const int tid){
        const int producer = tid % producers;    //get the current producer

        //use 2 for loops to avoid modulo arithmetic
        for(int i = current_consumer; i < consumers; i++){
            if((queueMatrix[producer][i])->push(item)){
                current_consumer = ((i + 1) == producers)? 0 : i;   //update the current index
                return true;
            }
        }
        for(int i = 0; i < current_consumer; i++){
            if((queueMatrix[producer][i])->push(item)){
                current_consumer = i;   //update the current index [no need for modulo branch since it will always be less]
                return true;
            }
        }
        //To keep load balancing next push will check from the last scanned consumer
        return false;
    }

    __attribute__((used,always_inline)) inline T* pop(const int tid){
        const int consumer = tid % consumers;    //get the current consumer

        T* item = nullptr;

        //use 2 for loops to avoid modulo arithmetic
        for(int i = current_producer; i < producers; i++){
            item = (queueMatrix[i][consumer])->pop();
            if(item != nullptr){
                //update producer idx 
                current_producer = ((i + 1) == producers)? 0 : i;   //update the current index
                return item;
            }
        }

        for(int i = 0; i < current_producer; i++){
            item = (queueMatrix[i][consumer])->pop();
            if(item != nullptr){
                current_producer = i;   //update the current index [no need for modulo branch since it will always be less]
                return item;
            }
        }
        //To keep load balancing next pop will check from the last scanned producer
        return item;
    }
};