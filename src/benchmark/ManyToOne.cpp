#include <string>
#include <vector>
#include <iostream>
#include <barrier>
#include <chrono>
#include <type_traits>
#include <optional>
#include "QueueTypeSet.hpp"
#include "NumaDispatcher.hpp"
#include "AdditionalWork.hpp"

#define PRINT_CLUSTER false //debugging
//#define DEBUG 0 //[uncomment to check queue correctness]
//#define DISABLE_AFFINITY 0 // [uncomment to disable affinity]

#ifdef DEBUG
#include <cassert>
#endif

//Parameters for exponential delay
#define MIN_BACKOFF     2048ull //~250ns
#define MAX_BACKOFF     32768ull //~4us
/**
 * On low contention enviroment a push/pop operation can last 500ns
 */

#define CACHE_LEVEL     3
#define NSEC_IN_SEC     1'000'000'000ull

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
    size_t items;
    std::barrier<>* threadsBarrier = nullptr;                  //all threads + main
    std::barrier<>* producersBarrier = nullptr;                //only producers + main
    std::barrier<>* consumersBarrier = nullptr;                //only consumers + main
    std::vector<std::vector<Data>> *itemsPerProducer = nullptr; //only used if DEBUG is defined
    std::vector<uint64_t> *itemsPerConsumer = nullptr; //only used if DEBUG is defined
    std::atomic<bool> *stopFlag = nullptr;                //used by consumers [setted by main]
    std::vector<int>  *threadCluster = nullptr;            //only used if DEBUG is defined and affinity is enabled
    //parameters for random work
    size_t center;
    size_t amplitude;
    threadShared() = default;
};

/**
 * We use a void * so we can either pach an std::vector ptr or an integer
 */
template<template<typename> typename Q>
void producer_routine(Q<Data> &queue, size_t items, threadShared &sharedArgs, const int tid);
template<template<typename> typename Q>
void consumer_routine(Q<Data> &queue, threadShared &sharedArgs, const int tid);
template<template<typename> typename Q>
long double benchmark(size_t producers, size_t consumers, size_t sizeQueue, size_t items,size_t center,size_t amplitude);





int main(int argc, char **argv) {
    
    if(argc == 2){  //Useful to check if a queue name is valid
        Queues::foreach([&argv]<template <typename> typename Q>() {
        std::string queueName = Q<int>::className(false);   //the type int is irrelevant
            if (std::string(argv[1]) == queueName) {
                exit(0);
            }
        });
        return 1;
    }

    if(argc != 8){
        std::cerr << "Usage: " << argv[0] << " <queue_name> <producers> <consumers> <size_queue> <items> <rand_center> <rand_amplitude>" << std::endl;
        return 1;
    }

    //refers to the class name of the queue [discarding the /padding suffix if present]
    std::string name    = argv[1];
    size_t producers    = std::stoul(argv[2]);
    size_t consumers    = std::stoul(argv[3]);
    size_t sizeQueue    = std::stoul(argv[4]);
    size_t duration     = std::stoul(argv[5]);
    size_t center       = std::stoul(argv[6]);
    size_t amplitude    = std::stoul(argv[7]);

    /**
     * Perform a foreach operation over the Queues template set and benchmark the queue
     * @note Queues defined in QueueTypeSet.hpp
     * @note if the same queue is contained more than once in the set, the benchmark is executed only one time
     */
    Queues::foreach([&]<template <typename> typename Q>() { //captures all the outer scope
        std::string queueName = Q<int>::className(false);   //the type int is irrelevant
        if (name == queueName) {
            std::cout << 
#ifdef DEBUG
            "DEBUG: " <<
#endif
            benchmark<Q>(producers,consumers, sizeQueue, duration, center, amplitude) << std::endl;
            exit(0); //exit the program successfully
        }
    });

        std::cerr << "Queue not found: " << name << std::endl;
        return 1;
}






template<template<typename> typename Q>
long double benchmark(size_t producers, size_t consumers, size_t sizeQueue, size_t items,size_t center,size_t amplitude){
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
    sharedArgs.center = center;
    sharedArgs.amplitude = amplitude;


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
    std::vector<uint64_t> itemsPerConsumer(consumers,0);

    size_t curr_assignment = 1; //we start from value 1 to insert to value items + 1

    for(int i = 0; i < producers; i++){
        const size_t size = producerBatch + (i < remainer ? 1 : 0);
        producersMatrix[i].reserve(size);   //init data as default
        for(int j = 0; j < size; j++){
            producersMatrix[i].push_back(Data(i,curr_assignment++));
        }
    }

    assert(curr_assignment == items+1); //to accout for the first value

    //set the shared arguments
    sharedArgs.itemsPerProducer = &(producersMatrix);
    sharedArgs.itemsPerConsumer = &(itemsPerConsumer);
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
        producer_threads.emplace_back( producer_routine<Q>,std::ref(queue), 
                                        producerBatch + (current_tid < remainer ? 1 : 0), //load balance
                                        std::ref(sharedArgs),
                                        current_tid);
        current_tid++;
    }

    //schedule consumers
    while(current_tid < producers + consumers){
        consumer_threads.emplace_back( consumer_routine<Q>,std::ref(queue),
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
            std::cerr << "Cluster Error: Thread " << i << "not assigned. Cluster: " << threadClusterAssignment[i] << "\n";
            clusterSetError = true; 
        } else {    //so messages don't overlap
#ifdef DEBUG
#if PRINT_CLUSTER
        std::cout << "Thread " << i << " running on Cluster " << threadClusterAssignment[i] << "\n";
#endif
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
    stopFlag.store(true,std::memory_order_release);
    allThreads.arrive_and_wait(); //wait for all threads to be done
    auto end = std::chrono::high_resolution_clock::now();
    for(auto &prod : producer_threads)
        prod.join();
    for(auto &cons : consumer_threads)
        cons.join();

#ifdef DEBUG    //check for correct delivery
    uint64_t totalTransfers = std::accumulate(itemsPerConsumer.begin(),itemsPerConsumer.end(),0);
    if(items != totalTransfers){
        std::cerr << "ERROR: Sent Items " << items << " != " << totalTransfers << " Received Items\n";
    }
#endif

    //return the ops per sec
    std::chrono::nanoseconds deltaTime = end - start;
    return static_cast<long double>(items * NSEC_IN_SEC) / deltaTime.count();
}





template<template<typename> typename Q>
void producer_routine(Q<Data> &queue, size_t items, threadShared &sharedArgs, const int tid){
    sharedArgs.threadsBarrier->arrive_and_wait(); //wait for main to set affinity

#ifndef DISABLE_AFFINITY    //if affinity has been set
    (*sharedArgs.threadCluster)[tid] = NumaDispatcher::get_numa_node();
    sharedArgs.threadsBarrier->arrive_and_wait();
#endif

    Data *item;
#ifdef DEBUG    //get the items to be sent
    items = (*sharedArgs.itemsPerProducer)[tid].size();
#else
    item = new Data();  //no initialization required because it won't be checked
#endif
    uint failed = 0;
    uint64_t delay = MIN_BACKOFF;    //initial delay
    const size_t center = sharedArgs.center;
    const size_t amplitude = sharedArgs.amplitude;
    const size_t producers = sharedArgs.producers;
    sharedArgs.threadsBarrier->arrive_and_wait();
    for(size_t i = 0; i < items; i++){
        random_work(center,amplitude);   //simulate random work [between minWait and maxWait]
        

#ifdef DEBUG    //use the item from the preallocated vector
        item = &((*(sharedArgs.itemsPerProducer))[tid][i]);
#endif
        //Bounded Queue push
        if constexpr (BoundedQueues::Contains<Q>){
            while(!(queue.push(item,tid))){
                loop(delay);
                delay <<= 1;
                delay = ((delay - 1) & (MAX_BACKOFF - 1)) + 1; //clamped bound
            }
            delay = MIN_BACKOFF;    //reset the delay
        } else {
            queue.push(item,tid);
        }
    }

    sharedArgs.producersBarrier->arrive_and_wait();  //main can notify consumers to drain the queue
    sharedArgs.threadsBarrier->arrive_and_wait();    //All threads are done by now
#ifndef DEBUG
    delete item;    //delete the preallocated item [if not in debug mode]
#endif
    return;
}






inline void consumer_check(std::vector<size_t>& lastSeen, const Data* item){
    if(lastSeen[item->tid] >= item->value){
        std::cerr << "ERROR: Producer " << item->tid << " sent item " << item->value << " after " << lastSeen[item->tid] << std::endl;
        exit(1);
    }
    lastSeen[item->tid % lastSeen.size()] = item->value;
}






template<template<typename> typename Q>
void consumer_routine(Q<Data> &queue, threadShared &sharedArgs, const int tid){
    sharedArgs.threadsBarrier->arrive_and_wait(); //wait for main to set affinity

#ifndef DISABLE_AFFINITY    //check with main that pinning was successful;
    (*sharedArgs.threadCluster)[tid] = NumaDispatcher::get_numa_node();
    sharedArgs.threadsBarrier->arrive_and_wait();
#endif

#ifdef DEBUG    //if DEBUG then we check for total number of transfers and FIFO ordering amongst producers
    std::vector<size_t> lastSeen(sharedArgs.producers,0);   //last seen value for each producer
    uint64_t transfer = 0;  //number of items transferred
#endif

    Data *item = nullptr;
    uint64_t delay = MIN_BACKOFF;    //initial delay
    const size_t center = sharedArgs.center;
    const size_t amplitude = sharedArgs.amplitude;
    const size_t producers = sharedArgs.producers;
    sharedArgs.threadsBarrier->arrive_and_wait();
    
    while(!sharedArgs.stopFlag->load(std::memory_order_relaxed)){   //the flag value is updated with store(std::memory_order_release)
        item = queue.pop(tid);
        //pop with delay if unsuccessful
        if(item == nullptr){
            loop(delay);
            delay <<= 1;
            delay = ((delay - 1) & (MAX_BACKOFF - 1)) + 1; //clamped bound
        } else delay = MIN_BACKOFF;    //reset the delay

#ifdef DEBUG    //the item gets checked if not nullptr
        if(item != nullptr){
            ++transfer;
            consumer_check(lastSeen,item);  //bound to fail if item out of order [else updates lastSeen]
        }
#endif
        for(size_t i = 0; i < producers; i++)
            random_work(center,amplitude);   //simulate random work [between minWait and maxWait]
    }

    //Queue draining
    do{
        item = queue.pop(tid);  //since all producers are done no need of delay
#ifdef DEBUG
        if(item != nullptr){    //check if the item is in order
            ++transfer;
            consumer_check(lastSeen,item);  //bound to fail if item out of order [else updates lastSeen]
        }
#endif
        if(item == nullptr) break;
        for(size_t i = 0; i < producers; i++)
            random_work(center,amplitude);   //simulate random work [between minWait and maxWait]
    }while(true);

    sharedArgs.threadsBarrier->arrive_and_wait();  //notify main that consumers are done [stops the measure]

#ifdef DEBUG    //set the transfer value before exiting
    (*sharedArgs.itemsPerConsumer)[tid % sharedArgs.consumers] = transfer;  //guarantees mod access to vector
#endif
    return;
}