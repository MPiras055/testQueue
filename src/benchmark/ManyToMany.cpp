#include <iostream>
#include <string>
#include <chrono>
#include <barrier>
#include <thread>
#include <chrono>
#include "QueueTypeSet.hpp"
#include "AdditionalWork.hpp"
#include "ThreadStruct.hpp"

#define MAX_BACKOFF 10

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
                if(backoff++ >= MAX_BACKOFF){
                    std::this_thread::sleep_for(std::chrono::nanoseconds{50});
                }
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

/**
 * @brief Consumer Thread routine for benchmark
 * 
 * @note if DEBUG defined then checks queue semantics
 * @note 2-stage waiting synchronization because consumers can exit only when producer is done 
 * and queue is empty
 */
template<template<typename> typename Q>
void consumerRoutine(Q<Data> *queue, threadArgs *args, size_t *transfers, std::vector<size_t> *lastSeen_param, const int tid){ //passo io il puntatore a last seen
    const size_t min_wait = args->min_wait;
    const size_t max_wait = args->max_wait;
    size_t consumerTransfer = 0;
#ifdef DEBUG
    /*  each producer gets a size_t vector to check queue semantics over
        items inserted by all producers 
    */
    //std::vector<size_t> lastSeen(args->producers,0);  
    std::vector<size_t> &lastSeen = *lastSeen_param;  //reference to avoid out of scope access
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
        random_work(min_wait,max_wait);
    }
    //producer done: drain the queue
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
    *transfers = consumerTransfer;  //load the transfer value to main variable
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
    std::vector<std::thread> threads;

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

    std::vector<std::vector<size_t>> lastSeenMatrix(consumers,std::vector<size_t>(producers,0));


    //schedule producers
    for(int tid = 0; tid < producers ; tid++){
        threads.emplace_back(producerRoutine<Q>,queue,&arg,producerItems[tid],tid);
    }

    //schedule consumer
    std::vector<size_t> consumerResult(consumers,0);
    for(int tid = producers; tid < producers + consumers ; tid++){
        int tid_consumer = tid - producers;
        threads.emplace_back(consumerRoutine<Q>,queue,&arg,&consumerResult[tid-producers],&(lastSeenMatrix[tid_consumer]),tid_consumer);
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
    delete queue;

#ifdef DEBUG
    /*  assert that the sum of the items dequeued by each consumer 
        is the same as the beginning item count */
    size_t totalTransfers = 0;
    for(auto &t : consumerResult){
        totalTransfers += t;
    }
    
    if(totalTransfers != items){
        //calculate the last item seen from each producer thread
        std::vector<size_t> lastSeenProducer(producers,0);
        for(int i = 0; i < consumers; i++){
            for(int j = 0; j < producers; j++){
                if(lastSeenMatrix[i][j] > lastSeenProducer[j]){
                    lastSeenProducer[j] = lastSeenMatrix[i][j];
                }
            }
        }

        //stampa il vettore
        std::cerr << "Last seen: ";
        for(auto &i : lastSeenProducer){
            std::cerr << i << " ";
        }
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