#include <string>
#include <iostream>
#include <barrier>
#include <chrono>
#include "QueueTypeSet.hpp"
#include "NumaDispatcher.hpp"
#include "AdditionalWork.hpp"

//#define DEBUG 0 //[uncomment to check queue correctness]
//#define DISABLE_AFFINITY 0 // [uncomment to disable affinity]

#ifdef DEBUG
#include <cassert>
#include <functional>
#include <algorithm>
#include <numeric>
#include <set>
#endif

#define MAX_BACKOFF_DURATION 150ul //in nanoseconds
#define CACHE_LEVEL 3
#define NSEC_IN_SEC 1'000'000'000ULL

struct Data {
    int tid;
    size_t value;
    Data() = default;
    Data(int tid,size_t val): tid(tid), value(val){};
    void set(int tid_p,size_t val_p){
        tid = tid_p;
        value = val_p;
    }
    auto operator <=> (const Data&) const = default;
};


/**
 * The struct holds all the shared arguments between threads
 * 
 * @note we use 2 std::vector (matrices) when debug is defined to check the correctness of the queue
 * @note the vectors will be passed as references
 * @note all arguments are passed as references
 */
struct threadShared {
    size_t producers;
    size_t consumers;
    std::barrier<>* threadsBarrier = nullptr;                  //all threads + main
    std::barrier<>* producersBarrier = nullptr;                //only producers + main
    std::barrier<>* consumersBarrier = nullptr;                //only consumers + main
    std::vector<std::vector<Data>> *itemsPerProducer = nullptr; //only used if DEBUG is defined
    std::vector<std::vector<Data>> *itemsPerConsumer = nullptr; //only used if DEBUG is defined
    std::atomic<bool> *stopFlag = nullptr;                //used by consumers [setted by main]
    std::vector<int>  *threadCluster = nullptr;            //only used if DEBUG is defined and affinity is enabled
    threadShared() = default;
};

/**
 * We use a void * so we can either pach an std::vector ptr or an integer
 */
template<template<typename> typename Q>
void producer_routine(Q<Data> &queue, size_t items, size_t minWait, size_t maxWait, threadShared &sharedArgs, const int tid);
template<template<typename> typename Q>
void consumer_routine(Q<Data> &queue, size_t minWait, size_t maxWait, threadShared &sharedArgs, const int tid);

//main benchmark function
template<template<typename> typename Q>
long double benchmark(size_t producers, size_t consumers, size_t sizeQueue, size_t items,size_t minWait,size_t max_wait);

int main(int argc, char **argv) {
    
    if(argc != 8){
        std::cerr << "Usage: " << argv[0] << " <queue_name> <producers> <consumers> <size_queue> <items> <min_wait> <max_wait>" << std::endl;
        return 1;
    }

    //refers to the class name of the queue [discarding the /padding suffix if present]
    std::string name = argv[1];
    size_t producers    = std::stoul(argv[2]);
    size_t consumers    = std::stoul(argv[3]);
    size_t sizeQueue    = std::stoul(argv[4]);
    size_t duration     = std::stoul(argv[5]);
    size_t minWait      = std::stoul(argv[6]);
    size_t maxWait      = std::stoul(argv[7]);

    bool found = false;
    long double result = 0;

    /**
     * Perform a foreach operation over the Queues template set and benchmark the queue
     * @note Queues defined in QueueTypeSet.hpp
     * @note if the same queue is contained more than once in the set, the benchmark is executed only one time
     */
    Queues::foreach([&]<template <typename> typename Q>() { //captures all the outer scope
        std::string queueName = Q<int>::className(false);   //the type int is irrelevant
        if (!found && name == queueName) {
            found = true;
            result = benchmark<Q>(producers,consumers, sizeQueue, duration, minWait, maxWait);
        }
    });

    if (!found) {
        std::cerr << "Queue not found: " << name << std::endl;
        return 1;
    }

    std::cout << result << std::endl;   //used to print result to stdout or to be piped by an automated .py script

    return 0;
}

template<template<typename> typename Q>
long double benchmark(size_t producers, size_t consumers, size_t sizeQueue, size_t items,size_t minWait,size_t max_wait){
    if(producers == 0 || consumers == 0 || sizeQueue == 0 || items == 0){
        std::cerr << "Error: Invalid null arguments" << std::endl;
        exit(1);
    }

    //init variables
    Q<Data> queue = Q<Data>(sizeQueue, producers + consumers); //queue only used by subthreads
    //init synch primitives
    std::barrier<> allThreads(producers + consumers + 1); //+1 for main
    std::barrier<> producersBarrier(producers + 1); //+1 for main
    std::barrier<> consumersBarrier(consumers + 1); //+1 for main
    std::atomic<bool> stopFlag(false); //used by consumers to stop the loop
    threadShared sharedArgs;  //shared arguments between threads

    //initialize the shared arguments
    sharedArgs.producers = producers;
    sharedArgs.consumers = consumers;
    sharedArgs.threadsBarrier = &allThreads;
    sharedArgs.producersBarrier = &producersBarrier;
    sharedArgs.consumersBarrier = &consumersBarrier;
    sharedArgs.stopFlag = &stopFlag;


    //load balance for producers
    const size_t producerBatch = items / producers;
    const size_t remainer = items % producers;  //to the first [remainer producers we add 1 item]

#ifdef DEBUG
    /**
     * If debug is defined then we preallocate all the items to be sent by the producers
     * so at the end we can check if the ordering is right and if all items are delivered
     * 
     * We also preallocate a storage space for the consumers to save the items, at the end we will
     * check if all items are received in the right order
     * 
     * All items contains a tid_stamp and an incremental value (starting from 0).
     * 
     * @note the assignment respect the load balance between producers
     */
    std::vector<std::vector<Data>> producersMatrix(producers);
    std::vector<std::vector<Data>> consumersMatrix(consumers);  //here vectors are initialized as empty

    size_t curr_assignment = 0;

    for(int i = 0; i < producers; i++){
        const size_t size = producerBatch + (i < remainer ? 1 : 0);
        producersMatrix[i].reserve(size);   //init data as default
        for(int j = 0; j < size; j++){
            producersMatrix[i].push_back(Data(i,curr_assignment++));
        }
    }

    assert(curr_assignment == items);

    //set the shared arguments
    sharedArgs.itemsPerProducer = &(producersMatrix);
    sharedArgs.itemsPerConsumer = &(consumersMatrix);
#endif
#ifndef DISABLE_AFFINITY
    //initialize the threadCluster vector
    std::vector<int> threadClusterAssignment(producers + consumers,-1);
    sharedArgs.threadCluster    = &(threadClusterAssignment);
#endif

    std::vector<std::thread> producer_threads;
    std::vector<std::thread> consumer_threads;

    //schedule producers
    int current_tid = 0;
    while(current_tid < producers){
        producer_threads.emplace_back( producer_routine<Q>, std::ref(queue), 
                                        producerBatch + (current_tid < remainer ? 1 : 0), //load balance
                                        minWait, max_wait,
                                        std::ref(sharedArgs),
                                        current_tid);
        current_tid++;
    }

    //schedule consumers
    while(current_tid < producers + consumers){
        consumer_threads.emplace_back( consumer_routine<Q>, std::ref(queue), 
                                        minWait, max_wait,
                                        std::ref(sharedArgs),
                                        current_tid);
        current_tid++;
    }

    /**
     * if the numa optimization isn't disabled then we
     */
#ifndef DISABLE_AFFINITY
    NumaDispatcher dispatcher(CACHE_LEVEL);
    /**
     * The dispatchment is done prioritizing filling cluster in a fair way between
     * producers and consumers (ratio based pinning). The threads are also dispatched
     * in a way to boost shared cache usage.
     */
    dispatcher.dispatch_threads(producer_threads,consumer_threads);
#endif

    //threads are ready
    allThreads.arrive_and_wait(); //threads wait [if affinity is enabled] for the main to set the affinity
#ifndef DISABLE_AFFINITY
    allThreads.arrive_and_wait(); //wait for all threads to have set their cluster field
    bool clusterSetError = false;

    for(int i = 0; i < threadClusterAssignment.size(); i++){
        if(threadClusterAssignment[i] < 0){
            std::cerr << "Cluster Error: Thread " << i << threadClusterAssignment[i] << "\n";
            clusterSetError = true; 
        } else {    //so messages don't overlap
#ifdef DEBUG
        std::cout << "Thread " << i << " running on Cluster " << threadClusterAssignment[i] << "\n";
#endif
        }
    }
    std::cout.flush(); //flush output
    assert(!clusterSetError); //abort if clusterFail

#endif
    //threads make initializations ...
    allThreads.arrive_and_wait();
    //Measuration
    auto start = std::chrono::high_resolution_clock::now();
    producersBarrier.arrive_and_wait();
    //set consumers flag
    stopFlag.store(true,std::memory_order_relaxed);
    allThreads.arrive_and_wait(); //wait for all threads to be done
    auto end = std::chrono::high_resolution_clock::now();
    for(auto &prod : producer_threads)
        prod.join();
    for(auto &cons : consumer_threads)
        cons.join();

#ifdef DEBUG    //check for correct delivery
    std::cout << "Asserting delivery: ";
    std::cout.flush();
    //check if data is correctly dequeued
    for(std::vector<Data> &consReceived : consumersMatrix){
        //stable sort [on tid] to keep the underlying value ordering
        std::stable_sort(consReceived.begin(),consReceived.end(),[](const Data& a, const Data& b){return a.tid < b.tid;});
        for(size_t i = 1; i < consReceived.size(); i++){
            const Data& deq1 = consReceived[i-1];
            const Data& deq2 = consReceived[i];
            if(deq1.tid == deq2.tid && !(deq1.value < deq2.value)){ //check for ordering
                std::cerr << "OUT_OF_ORDER: TID " << deq1.tid
                << " " << deq1.value << " "
                << deq2.value << "\n";
                exit(1);
            }
        }
    }

    //check if all transfered items match
    std::multiset<Data> producerItems;
    std::multiset<Data> consumerItems;
    for (const auto& data : producersMatrix)
        producerItems.insert(data.begin(), data.end());
    for (const auto& data : consumersMatrix)
        consumerItems.insert(data.begin(), data.end());

    if(producerItems.size() != consumerItems.size()){
        std::cerr << "ERROR: Items size not matching " << 
        "Producer: " << producerItems.size() << " Consumer: " << consumerItems.size() << std::endl;
        //determinates which thread put more items
        //we force all values in a signle vector, sort it and see for duplicates
        std::vector<Data> allItems;
        for(const std::vector<Data>& data : consumersMatrix){
            allItems.insert(allItems.end(),data.begin(),data.end());
        }
        std::sort(allItems.begin(),allItems.end(),[](const Data& a, const Data& b){return a.value < b.value;});
        for(size_t i = 1; i < allItems.size(); i++){
            const Data &prev = allItems[i-1];
            const Data &curr = allItems[i];
            if(prev.value == curr.value){
                std::cerr << "DUPLICATE: " << prev.value <<" TID " << prev.tid << " Curr " << curr.value << " TID " << curr.tid << std::endl;
            }
        }
        exit(1);
    }

    if(producerItems != consumerItems){
        std::cerr << "ERROR: Items not matching" << std::endl;
        exit(1);
    }
#endif

    //return the ops per sec
    std::chrono::nanoseconds deltaTime = end - start;
    return static_cast<long double>(items * NSEC_IN_SEC) / deltaTime.count();
}
template<template<typename> typename Q>
void producer_routine(Q<Data> &queue, size_t items, size_t minWait, size_t maxWait, threadShared &sharedArgs, const int tid){
    sharedArgs.threadsBarrier->arrive_and_wait(); //wait for main to set affinity
#ifndef DISABLE_AFFINITY    //if affinity has been set
    (*sharedArgs.threadCluster)[tid] = NumaDispatcher::get_numa_node();
    sharedArgs.threadsBarrier->arrive_and_wait();
#endif
    Data *item;
    size_t itemsSize = items;
#ifdef DEBUG    //get the items to be sent
    itemsSize = (*sharedArgs.itemsPerProducer)[tid].size();
#else
    item = new Data();  //no initialization required because it won't be checked
#endif
    sharedArgs.threadsBarrier->arrive_and_wait();
    for(size_t i = 0; i < itemsSize; i++){
        random_work(minWait,maxWait);   //simulate random work [between minWait and maxWait]
#ifdef DEBUG    //use the item from the preallocated vector
        item = &((*(sharedArgs.itemsPerProducer))[tid][i]);
#endif
        //Bounded Queue push
        if constexpr (BoundedQueues::Contains<Q>){
            while(!queue.push(item,tid)){
                std::this_thread::sleep_for(std::chrono::nanoseconds(randint(MAX_BACKOFF_DURATION)));
            }
        } else {
            queue.push(item,tid);
        }
    }

    sharedArgs.producersBarrier->arrive_and_wait();  //main can notify consumers to drain the queue
    sharedArgs.threadsBarrier->arrive_and_wait();    //All threads are done by now
#ifndef DEBUG
    delete item;    //delete the item
#endif
    return;
}
template<template<typename> typename Q>
void consumer_routine(Q<Data> &queue, size_t minWait, size_t maxWait, threadShared &sharedArgs, const int tid){
    sharedArgs.threadsBarrier->arrive_and_wait(); //wait for main to set affinity
#ifndef DISABLE_AFFINITY    //if affinity has been set
    (*sharedArgs.threadCluster)[tid] = NumaDispatcher::get_numa_node();
    sharedArgs.threadsBarrier->arrive_and_wait();
#endif
    const size_t mod_vector = sharedArgs.consumers;
    Data *item = nullptr;
    sharedArgs.threadsBarrier->arrive_and_wait();
    while(!sharedArgs.stopFlag->load(std::memory_order_relaxed)){
        item = queue.pop(tid);
#ifdef DEBUG
        if(item != nullptr) //access the vector only using the tid can cause out of bounds
            (*sharedArgs.itemsPerConsumer)[tid % mod_vector].push_back(*item);
#endif
    }
    //producers are done so drain the queue
    do{
        item = queue.pop(tid);
#ifdef DEBUG
        if(item != nullptr)
            (*sharedArgs.itemsPerConsumer)[tid % mod_vector].push_back(*item);
#endif
        if(item == nullptr) break;
        random_work(minWait,maxWait);   //simulate random work [between minWait and maxWait]
    }while(true);
    sharedArgs.threadsBarrier->arrive_and_wait();  //notify main that consumers are done
    return;
}