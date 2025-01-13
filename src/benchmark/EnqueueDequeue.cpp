#include <iostream>
#include <thread>
#include <barrier>
#include <chrono>
#include "QueueTypeSet.hpp"
#include "AdditionalWork.hpp"

#define NSEC_IN_SEC 1'000'000'000ULL

struct Data{
    int tid;
    size_t val;
    Data() = default;
    Data(int tid,size_t val): tid(tid), val(val){};
};


/**
 * @brief Thread routine for benchmark
 * 
 * Each thread pushes and pulls from the queue;
 * 
 * @param queue (Q<Data>*) queue to use
 * @param threadBarrier (std::barrier<>*)
 * @param numOps (size_t) number of operations
 * @param min_wait (size_t) minimum wait time [busy waiting iterations]
 * @param max_wait (size_t) maximum wait time [busy waiting iterations]
 * @param threads (int) number of threads [to initialize the lastValue vector]
 * @param tid (int) thread id
 * 
 * If DEBUG enable each thread allocate dynamically each element to push putting its tid and 
 * a value from 1 to numOps. Each thread, when popping checks that the last value popped [FROM THE SAME THREAD]
 * is strictly lower than the current one.
 */
template<template <typename> typename Q>
void threadRoutine(Q<Data> * queue, std::barrier<>* threadBarrier, size_t numOps, size_t min_wait, size_t max_wait, int threads ,const int tid){
#ifndef DEBUG   
    Data item(tid,0);
#else
    std::vector<size_t> lastValue(threads,0);
    std::vector<Data> items(numOps);
    for(size_t i = 0; i< numOps; i++){
        items[i].tid = tid;
        items[i].val = i+1;
    }
#endif

    threadBarrier->arrive_and_wait();
    for(size_t i = 0; i < numOps; i++){
#ifdef DEBUG
    Data& item = items[i];
#endif
        if constexpr (BoundedQueues::Contains<Q>){
            while(!queue->push(&item,tid));
        } else {
            queue->push(&item,tid);
        }
        random_work(min_wait,max_wait);
#ifndef DEBUG
        queue->pop(tid);
#else
        Data *popped = queue->pop(tid);
        if(popped != nullptr){
            if(lastValue[popped->tid] >= popped->val){
                std::cerr << "Error at iteration: " << i << " Value: " << popped->val 
                << " Last Value: " << lastValue[popped->tid] << std::endl;
                exit(1);    //exit with error
            }
            lastValue[popped->tid] = popped->val;
        }  
#endif
    }
    threadBarrier->arrive_and_wait();
    return;
}


/**
 * @brief Benchmark function for a given queue
 * 
 * Counts the opsPerSec in terms of consecutive push/pop operations.
 * 
 * Each thread takes its own time of execution and the final result 
 * is the raw number of operations divided by the aggregated time
 * 
 * @param numThreads (size_t) number of threads
 * @param size_queue (size_t) size of the queue
 * @param numOps (size_t) number of operations
 * @param min_wait (int) minimum wait time [busy waiting iterations]
 * @param max_wait (int) maximum wait time [busy waiting iterations]
 * 
 * @returns (long double) operations per second
 * 
 * @note the function uses a barrier to synchronize the threads
 * @note threads perform random work between push and pop operations [min_wait,max_wait]
 *
 */
template <template <typename> typename Q>
long double benchmark(size_t numThreads,size_t size_queue, size_t numOps, size_t min_wait,size_t max_wait) {
    Q<Data> queue(size_queue, numThreads);
    std::barrier<> threadBarrier(numThreads + 1);

    std::vector<std::thread> threads;
    for (int tid = 0; tid < numThreads; tid++) {
        threads.emplace_back(threadRoutine<Q>,&queue, &threadBarrier, numOps, min_wait, max_wait, numThreads, tid);
    }

    threadBarrier.arrive_and_wait();
    auto start = std::chrono::high_resolution_clock::now();
    threadBarrier.arrive_and_wait();
    auto end = std::chrono::high_resolution_clock::now();
    for(auto &t : threads){
        t.join();
    }

    /**
     * Total Ops = numOps (for each thread) * 2 (push/pop) * numThreads
     */
    std::chrono::nanoseconds aggregated_time = end - start;
    long double opsPerSec = static_cast<long double>(numOps * 2 * numThreads * NSEC_IN_SEC) /(aggregated_time.count());

    return opsPerSec;
}

int main(int argc, char **argv) {
    if(argc < 6){
        std::cout << "Usage: " << argv[0] << " <queue_name> <num_threads> <size_queue> <num_ops> <min_wait> <max_wait>" << std::endl;
        return 1;
    }

    //refers to the class name of the queue [discarding the /padding suffix if present]
    std::string name = argv[1];
    size_t numThreads = std::stoul(argv[2]);
    size_t sizeQueue = std::stoul(argv[3]);
    size_t numOps = std::stoul(argv[4]);
    int minWait = std::stoul(argv[5]);
    int maxWait = std::stoul(argv[6]);

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
            result = benchmark<Q>(numThreads, sizeQueue, numOps, minWait, maxWait);
        }
    });

    if (!found) {
        std::cout << "Queue not found: " << name << std::endl;
        return 1;
    }
    std::cout << result << std::endl;
    return 0;
}