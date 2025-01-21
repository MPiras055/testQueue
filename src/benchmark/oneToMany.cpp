#include <iostream>
#include <string>
#include <chrono>
#include <barrier>
#include <thread>
#include "QueueTypeSet.hpp"
#include "AdditionalWork.hpp"
#include "ThreadStruct.hpp"

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
void producerRoutine(Q<Data> *queue, threadArgs *args, const int tid){
    const size_t min_wait = args->min_wait;
    const size_t max_wait = args->max_wait;
    const size_t iter = args->numOps;
#ifndef DEBUG
    Data item(tid,0);
#else
    Data item;
    std::vector<Data> items(iter);
    //initialize array so that it doesnt get reallocated
    for(size_t i = 0; i<iter ; i++){
        items[i].tid = tid;
        items[i].val = i+1;
    }
#endif
    (args->consumerBarrier)->arrive_and_wait(); //this is all_threads Barrier
    for(size_t i = 0; i < iter; i++){
        puts("IN HERE");
#ifdef DEBUG
        //Reference necessary to prevent out of scope access
        Data &item = items[i];
#endif
        if constexpr (BoundedQueues::Contains<Q>){
            if(!queue->push(&item,tid)){
                --i;
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
    //Used to check the queue semantics over items inserted by the producer
    size_t lastSeen = 0;
    Data *popped = nullptr;
    (args->consumerBarrier)->arrive_and_wait();
    while(!((args->stopFlag)->load())){
        popped = queue->pop(tid);
        if(popped != nullptr){
#ifdef DEBUG
            if(popped->val <= lastSeen){
                std::cerr << "Consumer " << tid << " received an out of order item: " << popped->val << " <= " << lastSeen << popped->tid << std::endl;
                exit(1);
            }
            lastSeen = popped->val;
#endif
            ++consumerTransfer;
        }
        random_work(min_wait,max_wait);
    }
    //Queue draining
    do{
        popped = queue->pop(tid);
        if(popped != nullptr){
#ifdef DEBUG
            if(popped->val <= lastSeen){
                std::cerr << "Consumer " << tid << " received an out of order item: " << popped->val << " from producer " << popped->tid << std::endl;
                exit(1);
            }
            lastSeen = popped->val;
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
long double benchmark(size_t consumers,size_t size_queue,size_t items,size_t min_wait,size_t max_wait){
    Q<Data> queue(size_queue, consumers + 1);
    puts("ENTERED");
    std::barrier<> threadBarrier(consumers + 2); //consumers + 1 producer + 1 main thread
    std::barrier<> producerBarrier(2);
    std::atomic<bool> stopFlag{false}; //flag to signal consumers that producer is done
    std::vector<size_t> consumerData(consumers);
    std::vector<std::thread> threads;

    threadArgs arg;
    arg.producerBarrier = &producerBarrier;
    arg.consumerBarrier = &threadBarrier;
    arg.stopFlag = &stopFlag;
    arg.min_wait = min_wait;
    arg.max_wait = max_wait;
    arg.producers = 1;
    arg.consumers = consumers;
    arg.numOps = items;

    //schedule producer
    puts("SCHEDULING THREADS");
    threads.emplace_back(producerRoutine<Q>,&queue,&arg,0);
    for(int tid = 1 ; tid < consumers + 1; tid++){
        threads.emplace_back(consumerRoutine<Q>,&queue,&arg,&(consumerData[tid-1]),tid);
    }
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
    size_t totalTransfers = 0;
    for(auto &t : consumerData){
        totalTransfers += t;
    }
    assert(totalTransfers == items);    //assert that all items have been correcty dequeued
#endif

    std::chrono::nanoseconds time = end - start;
    long double transfPerSec = static_cast<long double>(items * NSEC_IN_SEC) / static_cast<long double>((time).count());

    return transfPerSec;
}


int main(int argc, char **argv) {
    if(argc < 7){
        std::cout << "Usage: " << argv[0] << " <queue_name> <consumers> <size_queue> <items> <min_wait> <max_wait>" << std::endl;
        return 1;
    }

    puts("HELLO WORLD");

    //refers to the class name of the queue [discarding the /padding suffix if present]
    std::string name = argv[1];
    size_t consumers = std::stoul(argv[2]);
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
            result = benchmark<Q>(consumers, sizeQueue, duration, minWait, maxWait);
        }
    });

    puts("EXECUTED BENCHMARK");

    if (!found) {
        std::cout << "Queue not found: " << name << std::endl;
        return 1;
    }
    std::cout << "Hello world" << std::endl;
    std::cout << result << std::endl;
    return 0;
}