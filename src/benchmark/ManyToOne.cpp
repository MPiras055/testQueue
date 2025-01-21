#include <iostream>
#include <string>
#include <chrono>
#include <barrier>
#include <thread>
#include "QueueTypeSet.hpp"
#include "AdditionalWork.hpp"
#include "ThreadStruct.hpp"

//#define DEBUG 0

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
#ifndef DEBUG
    Data item(tid,0);
#else
    Data item;
    std::vector<Data> items(data);
    //Initialize array so that it doesnt get reallocated
    for(size_t i = 0; i<data ; i++){  
        items[i].tid = tid;
        items[i].val = i+1;
    }
#endif
    (args->consumerBarrier)->arrive_and_wait(); //this is all_threads Barrier
    for(size_t i = 0; i < data; i++){
#ifdef DEBUG
        //reference necessary to avoid out of scope access
        Data &item = items[i];  
#endif
        bool res;
        if constexpr (BoundedQueues::Contains<Q>){
            res = queue->push(&item,tid);
            if(!res){
                --i;    //try new iteration (do random_work before)
            }
        } else {
            queue->push(&item,tid);
        }
        random_work(min_wait,max_wait);
    }
    (args->producerBarrier)->arrive_and_wait();
    (args->consumerBarrier)->arrive_and_wait();
    return;
}

/**
 * @brief Consumer Thread routine for benchmark
 * 
 * @note if DEBUG defined then checks queue semantics
 * @note 2-stage waiting synchronization because consumers can exit only when producer is done 
 * and queue is empty
 */
template<template<typename> typename Q>
void consumerRoutine(Q<Data> *queue, threadArgs *args, size_t *transfers, const int tid){
    const size_t min_wait = args->min_wait;
    const size_t max_wait = args->max_wait;
    size_t consumerTransfer = 0;
#ifdef DEBUG
    /**
     * Each consumer gets a vector od size_t to check queue semantics over items inserted by the producers
     */
    std::vector<size_t> lastSeen(args->producers,0); 
#endif
    Data *popped = nullptr;

    (args->consumerBarrier)->arrive_and_wait();
    while(!((args->stopFlag)->load())){
        popped = queue->pop(tid);
        if(popped != nullptr){
#ifdef DEBUG
            if(popped->val <= lastSeen[popped->tid]){
                std::cerr << "Consumer " << tid << " received an out of order item: " << popped->val << " <= " << lastSeen[popped->tid] << std::endl;
                exit(1);
            }
            lastSeen[popped->tid] = popped->val;
#endif
            ++consumerTransfer;
        }
        //random_work(min_wait,max_wait);
    }
    //Queue draining
    do{
        popped = queue->pop(tid);
        if(popped != nullptr){
#ifdef DEBUG
            if(popped->val <= lastSeen[popped->tid]){
                std::cerr << "Consumer " << tid << " received an out of order item: " << popped->val << " from producer " << popped->tid << std::endl;
                exit(1);
            }
            lastSeen[popped->tid] = popped->val;
#endif
            ++consumerTransfer;
        }
        random_work(min_wait,max_wait);
    }while(popped != nullptr);
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
long double benchmark(size_t producers,size_t size_queue,size_t items,size_t min_wait,size_t max_wait){
    Q<Data> queue(size_queue, producers + 1);
    std::barrier<> threadBarrier(producers + 2); // (producers + 1 producer + 1 main thread)
    std::barrier<> producerBarrier(producers + 1);
    std::atomic<bool> stopFlag{false}; //flag read by consumer [consumer is done after all producers are done]
    std::vector<std::thread> threads;

    threadArgs arg;
    arg.producerBarrier = &producerBarrier;
    arg.consumerBarrier = &threadBarrier;
    arg.stopFlag = &stopFlag;
    arg.min_wait = min_wait;
    arg.max_wait = max_wait;
    arg.producers = producers;
    arg.consumers = 1;
    arg.numOps = items;

    // [producer load balance]
    size_t itemPerProducer = items / producers;
    size_t remaining = items % producers;
    std::vector<size_t> producerItems(producers,itemPerProducer);
    for(int i = 0; i< remaining; i++){
        producerItems[i]++;
    }

    //schedule producers
    for(int tid = 0; tid < producers ; tid++)
        threads.emplace_back(producerRoutine<Q>,&queue,&arg,producerItems[tid],tid);

    //schedule consumer
    size_t consumerResult = 0;  //Only used in DEBUG mode
    threads.emplace_back(consumerRoutine<Q>,&queue,&arg,&consumerResult,producers);

    threadBarrier.arrive_and_wait();
    auto start = std::chrono::high_resolution_clock::now();
    producerBarrier.arrive_and_wait();
    stopFlag.store(true);
    threadBarrier.arrive_and_wait();
    auto end = std::chrono::high_resolution_clock::now();
    for(auto &t : threads){
        t.join();
    }

#ifdef DEBUG
    assert(consumerResult == items);    //assert that all items have been correcty dequeued
#endif

    std::chrono::nanoseconds time = end - start;
    long double transfPerSec = static_cast<long double>(items * NSEC_IN_SEC) / static_cast<long double>((time).count());

    return transfPerSec;
}


int main(int argc, char **argv) {
    #ifdef DEBUG
    puts("DEBUG MODE");
    #endif
    if(argc < 7){
        std::cout << "Usage: " << argv[0] << " <queue_name> <producers> <size_queue> <items> <min_wait> <max_wait>" << std::endl;
        return 1;
    }

    //refers to the class name of the queue [discarding the /padding suffix if present]
    std::string name = argv[1];
    size_t producers = std::stoul(argv[2]);
    size_t sizeQueue = std::stoul(argv[3]);
    size_t duration = std::stoul(argv[4]);
    size_t minWait = std::stoul(argv[5]);
    size_t maxWait = std::stoul(argv[6]);

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
            result = benchmark<Q>(producers, sizeQueue, duration, minWait, maxWait);
        }
    });

    if (!found) {
        std::cout << "Queue not found: " << name << std::endl;
        return 1;
    }
    std::cout << result << std::endl;
    return 0;
}