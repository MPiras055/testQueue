#include <iostream>
#include <string>
#include <chrono>
#include <barrier>
#include <thread>
#include "QueueTypeSet.hpp"
#include "AdditionalWork.hpp"
#include "ThreadStruct.hpp"



/**
 * @brief Thread routine for benchmark
 * 
 * @note doesn't do anything on DEBUG since the number of items that get
 * pushed is not known
 */
template<template<typename> typename Q>
void producerRoutine(Q<Data> *queue, threadArgs *args, const int tid){
    const size_t min_wait = args->min_wait;
    const size_t max_wait = args->max_wait;
    Data item(tid,0);
    (args->producerBarrier)->arrive_and_wait();
    while(!args->stopFlag->load()){
        if constexpr (BoundedQueues::Contains<Q>){
            while(!queue->push(&item,tid)){
                //have to have a exit mechanism here because producers could hang up
            };
        } else {
            queue->push(&item,tid);
        }
        random_work(min_wait,max_wait);
    }
    (args->producerBarrier)->arrive_and_wait();
    return;
}

template<template<typename> typename Q>
void consumerRoutine(Q<Data> *queue, threadArgs *args, size_t *transfers, const int tid){
    const size_t min_wait = args->min_wait;
    const size_t max_wait = args->max_wait;
    size_t successfulTransfers = 0;
    Data *popped;
    (args->consumerBarrier)->arrive_and_wait();
    while(!((args->stopFlag)->load())){
        popped = queue->pop(tid);
        if(popped != nullptr){
            successfulTransfers++;
        }
        random_work(min_wait,max_wait);
    }
    *transfers = successfulTransfers;
    (args->consumerBarrier)->arrive_and_wait();
    return;

}

/**
 * @brief Benchmark function for a given queue
 * 
 * Counts the number of successful transfer given a time duration
 * 
 */
template< template <typename> typename Q>
long double benchmark(size_t numProd,size_t numCons,size_t size_queue,size_t duration_sec,size_t min_wait,size_t max_wait){
    Q<Data> queue(size_queue, numProd + numCons);
    std::barrier<> threadBarrier(numProd + numCons + 1);
    std::atomic<bool> stopFlag{false};

    std::vector<std::thread> threads;
    std::vector<size_t> transfers = std::vector<size_t>(numCons,0);
    //cast the duration to seconds
    std::chrono::seconds duration(duration_sec);

    threadArgs arg;
    arg.producerBarrier = &threadBarrier;
    arg.consumerBarrier = &threadBarrier;
    arg.stopFlag = &stopFlag;
    arg.min_wait = min_wait;
    arg.max_wait = max_wait;
    arg.producers = numProd;
    arg.consumers = numCons;

    for(int tid = 0; tid < numProd; tid++){
        threads.emplace_back(producerRoutine<Q>,&queue,&arg,tid);
    }

    for(int tid = numProd; tid < numCons+numProd; tid++){
        //schedule consumers [remember to pass the pointer to the vector cell where to write results]
        threads.emplace_back(consumerRoutine<Q>,&queue,&arg,&(transfers[tid-numProd]),tid);
    }

    threadBarrier.arrive_and_wait();
    auto start = std::chrono::high_resolution_clock::now();
    std::this_thread::sleep_until(start + duration);
    stopFlag.store(true);
    auto stop = std::chrono::high_resolution_clock::now();
    threadBarrier.arrive_and_wait();
    for(auto &t : threads){
        t.join();
    }

    //collect results and divide by the time
    std::chrono::nanoseconds delta(stop - start);
    size_t totalTransfers = 0;
    for(size_t t : transfers){
        totalTransfers += t;
    }

    long double transferPerSec = static_cast<long double>(totalTransfers * NSEC_IN_SEC) /static_cast<long double>(delta.count());

    return transferPerSec;
}


int main(int argc, char **argv) {
    if(argc < 8){
        std::cout << "Usage: " << argv[0] << " <queue_name> <prods> <cons> <size_queue> <duration_sec> <min_wait> <max_wait>" << std::endl;
        return 1;
    }

    //refers to the class name of the queue [discarding the /padding suffix if present]
    std::string name = argv[1];
    size_t prods = std::stoul(argv[2]);
    size_t cons = std::stoul(argv[3]);
    size_t sizeQueue = std::stoul(argv[4]);
    size_t duration = std::stoul(argv[5]);
    size_t minWait = std::stoul(argv[6]);
    size_t maxWait = std::stoul(argv[7]);

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
            result = benchmark<Q>(prods, cons, sizeQueue, duration, minWait, maxWait);
        }
    });

    if (!found) {
        std::cout << "Queue not found: " << name << std::endl;
        return 1;
    }
    std::cout << result << std::endl;
    return 0;
}