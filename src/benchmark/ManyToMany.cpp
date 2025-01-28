#include <iostream>
#include <string>
#include <chrono>
#include <barrier>
#include <thread>
#include <chrono>
#include <atomic>
#include "QueueTypeSet.hpp"
#include "AdditionalWork.hpp"
#include "ThreadStruct.hpp"
#include <typeinfo>
#include <cxxabi.h>
#include <iostream>
#include <thread>
#include "NumaDispatcher.hpp"

#define MAX_BACKOFF 350ul //ns

#define CACHE_LEVEL 3u  //cache level optimization for dispatching

#define DEBUG 0

/**
 * @brief Producer Thread routine for benchmark
 * 
 * @note if DEBUG defined then checks queue semantics: allocs all items to push
 * in a RESERVED SPACE vector (so that it doesnt get reallocated)
 * 
 * @note double barrier wait at the end, because the vector can get out of scope
 * only when consumers are done using it. Otherwise: invalid access
 */
template<template<typename> typename Q>
void producerRoutine(Q<Data> *queue, threadArgs *args, size_t data, const int tid){
    const size_t min_wait = args->min_wait;
    const size_t max_wait = args->max_wait;

    //Only used for bounded queues
    uint64_t backoff = 0;


#ifndef DEBUG
    Data item(tid,0);
#else
    Data item;
    std::vector<Data> items(data);
    //Initialize vector so that it doesnt get reallocated
    for(size_t i = 0; i<data ; i++){
        items[i].tid = tid;
        items[i].val = i+1;
    }
#endif
    int j = 0;
    (args->consumerBarrier)->arrive_and_wait(); //all threads barrier
    for(size_t i = 0; i < data; i++){
        random_work(min_wait,max_wait);
#ifdef DEBUG
        //reference necessary to avoid out of scope access
        Data &item = items[i];  
#endif
        if constexpr (BoundedQueues::Contains<Q>){
            while(!queue->push(&item,tid)){
                std::this_thread::sleep_for(std::chrono::nanoseconds(randint(MAX_BACKOFF)));
            }; //retry until successful push
            backoff = 0;
        } else {
            queue->push(&item,tid);
        }
    }
    (args->producerBarrier)->arrive_and_wait();
    (args->consumerBarrier)->arrive_and_wait();
    return;
}

template<template<typename> typename Q>
void consumerRoutine(Q<Data> *queue, threadArgs *args, size_t *transfers, const int tid) {
    const size_t min_wait = args->min_wait;
    const size_t max_wait = args->max_wait;
    size_t consumerTransfer = 0;
    
    // Track last seen value per producer to verify ordering
    #ifdef DEBUG
    std::vector<size_t> lastSeen(args->producers, 0);
    #endif
    
    Data *popped = nullptr;
    
    // Wait for all threads to be ready
    (args->consumerBarrier)->arrive_and_wait();
    
    // Main processing loop while producers are active
    while(!((args->stopFlag)->load())) {
        popped = queue->pop(tid);
        if(popped != nullptr) {
            #ifdef DEBUG
            // Verify that values from each producer are strictly increasing
            if(popped->tid < args->producers) {  // Validate producer ID
                if(popped->val < lastSeen[popped->tid]) {
                    std::cerr << "Order violation: Consumer " << tid 
                             << " received " << popped->val 
                             << " after " << lastSeen[popped->tid]
                             << " from producer " << popped->tid << std::endl;
                    exit(1);
                }
                lastSeen[popped->tid] = popped->val;
            }
            #endif
            ++consumerTransfer;
        }
        random_work(min_wait, max_wait);
    }
    
    // Drain remaining items after producers are done
    while(true) {
        popped = queue->pop(tid);
        if(popped == nullptr) {
            break;  // Queue is empty
        }
        
        #ifdef DEBUG
        // Apply the same ordering check to remaining items
        if(popped->tid < args->producers) {
            if(popped->val < lastSeen[popped->tid]) {
                std::cerr << "Order violation during drain: Consumer " << tid 
                         << " received " << popped->val 
                         << " after " << lastSeen[popped->tid]
                         << " from producer " << popped->tid << std::endl;
                exit(1);
            }
            lastSeen[popped->tid] = popped->val;
        }
        #endif
        
        ++consumerTransfer;
        random_work(min_wait, max_wait);
    }
    
    // Final synchronization
    args->consumerBarrier->arrive_and_wait();
    *transfers = consumerTransfer;
    return;
}

/**
 * @brief Benchmark function for a given queue
 * 
 * Counts the number of successful transfer given a time duration
 * 
 */
template< template <typename> typename Q>
long double benchmark(size_t producers,size_t consumers,size_t size_queue,size_t items,size_t min_wait,size_t max_wait){
    Q<Data> * queue = new Q<Data>(size_queue, producers + consumers + 1);
    std::barrier<> threadBarrier(producers + consumers + 1); //(producers + 1 producer + 1 main thread)
    std::barrier<> producerBarrier(producers + 1);
    std::atomic<bool> stopFlag{false}; //flag to signal consumer that producers are done
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    threadArgs arg;
    arg.producerBarrier = &producerBarrier;
    arg.consumerBarrier = &threadBarrier;
    arg.stopFlag = &stopFlag;
    arg.min_wait = min_wait;
    arg.max_wait = max_wait;
    arg.producers = producers;
    arg.consumers = consumers;
    arg.numOps = items;

    // [Producer load balance]
    size_t itemPerProducer = items / producers;
    size_t remaining = items % producers;
    std::vector<size_t> producerItems(producers,itemPerProducer);
    for(int i = 0; i< remaining; i++){
        producerItems[i]++;
    }

    //schedule producers
    for(int tid = 0; tid < producers ; tid++){
        producers.emplace_back(producerRoutine<Q>,queue,&arg,producerItems[tid],tid);
    }

    //schedule consumer
    std::vector<size_t> consumerResult(consumers,0);
    for(int tid = producers; tid < producers + consumers ; tid++){
        int tid_consumer = tid - producers;
        consumers.emplace_back(consumerRoutine<Q>,queue,&arg,&consumerResult[tid-producers],tid_consumer);
    }
    NumaDispatcher numa(3);
    numa.dispatch_threads(producers,consumers);
    std::this_thread::sleep_for(std::chrono::hours{1});
    threadBarrier.arrive_and_wait();
    auto start = std::chrono::high_resolution_clock::now();
    producerBarrier.arrive_and_wait();
    stopFlag.store(true);
    threadBarrier.arrive_and_wait();
    auto end = std::chrono::high_resolution_clock::now();
    for(auto &t : producers){
        t.join();
    }
    for(auto &t : consumers){
        t.join();
    }
    delete queue;

#ifdef DEBUG
    /*  assert that the sum of the items dequeued by each consumer 
        is the same as the beginning item count */
    size_t totalTransfers = 0;
    for(auto &t : consumerResult){
        totalTransfers += t;
    }
    
    if(totalTransfers != items){
        std::cerr << std::endl;
        std::cerr << "Assertion failed: "  << totalTransfers << " != " << items << std::endl;
        exit(1);
    }
#endif

    std::chrono::nanoseconds time = end - start;
    long double transfPerSec = static_cast<long double>(items * NSEC_IN_SEC) / static_cast<long double>((time).count());

    return transfPerSec;
}


int main(int argc, char **argv) {
    return 0;

    if(argc != 8){
        std::cout << "Usage: " << argv[0] << " <queue_name> <producers> <consumers> <size_queue> <items> <min_wait> <max_wait>" << std::endl;
        return 1;
    }

    //refers to the class name of the queue [discarding the /padding suffix if present]
    std::string name = argv[1];
    size_t producers = std::stoul(argv[2]);
    size_t consumers = std::stoul(argv[3]);
    size_t sizeQueue = std::stoul(argv[4]);
    size_t duration = std::stoul(argv[5]);
    size_t minWait = std::stoul(argv[6]);
    size_t maxWait = std::stoul(argv[7]);

    if(duration == 0)
        return 0;

    bool found = false;
    long double result = 0;

    /**
     * Perform a foreach operation over the Queues template set and benchmark the queue
     * @note Queues defined in QueueTypeSet.hpp
     * @note if the same queue is contained more than once in the set, the benchmark is executed only one time
     */
    Queues::foreach([&]<template <typename> typename Q>() {
        std::string queueName = Q<int>::className(false);   //the type int is irrelevant
        if (!found && name == queueName) {
            found = true;
            result = benchmark<Q>(producers,consumers, sizeQueue, duration, minWait, maxWait);
        }
    });

    if (!found) {
        std::cout << "Queue not found: " << name << std::endl;
        return 1;
    }
    std::cout << result << std::endl;
    return 0;
}