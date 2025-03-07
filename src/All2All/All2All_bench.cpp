#include <string>
#include <vector>
#include <iostream>
#include <barrier>
#include <chrono>
#include <type_traits>
#include <optional>
#include "NumaDispatcher.hpp"
#include "AdditionalWork.hpp"
#include "All2All.hpp"
#include "TicksWait.h"

#define PRINT_CLUSTER false //debugging
//#define DEBUG 0 //[uncomment to check queue correctness]
//#define DISABLE_AFFINITY 0 // [uncomment to disable affinity]

#ifdef DEBUG
#include <cassert>
#endif


#define CACHE_LEVEL             3
#define NSEC_IN_SEC             1'000'000'000ull

#define CENTER_B_PUSH 512
#define AMPLITUDE_B_PUSH 256

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
    size_t prod_cons;
    std::barrier<>* threadsBarrier = nullptr;                   //all threads + main
    std::barrier<>* producersBarrier = nullptr;                 //only producers + main
    std::barrier<>* consumersBarrier = nullptr;                 //only consumers + main
    std::vector<std::vector<Data>> *itemsPerProducer = nullptr; //only used if DEBUG is defined
    std::vector<uint64_t> *itemsPerConsumer = nullptr;          //only used if DEBUG is defined
    std::atomic<bool> *stopFlag = nullptr;                      //used by consumers [setted by main]
    std::vector<int>  *threadCluster = nullptr;                 //only used if DEBUG is defined and affinity is enabled
    //parameters for random work
    size_t center;
    size_t amplitude;
    threadShared() = default;
};


void producer_routine(All2All<Data> &queue, size_t items, threadShared &sharedArgs, const int tid);

void consumer_routine(All2All<Data> &queue, threadShared &sharedArgs, const int tid);

long double benchmark(size_t producers, size_t consumers, size_t sizeQueue, size_t items,size_t center,size_t amplitude, size_t prod_cons);





int main(int argc, char **argv) {
    
    size_t prod_cons = 0;

    if(argc == 9)
        prod_cons = std::stoul(argv[8]);
    else if(argc != 8){
        std::cerr << "Usage: " << argv[0] << " <producers> <consumers> <size_queue> <items> <rand_center> <rand_amplitude> <prod_cons: default = 0>" << std::endl;
        return 1;
    }

    //refers to the class name of the queue [discarding the /padding suffix if present]
    size_t producers    = std::stoul(argv[1]);
    size_t consumers    = std::stoul(argv[2]);
    size_t sizeQueue    = std::stoul(argv[3]);
    size_t duration     = std::stoul(argv[4]);
    size_t center       = std::stoul(argv[5]);
    size_t amplitude    = std::stoul(argv[6]);

    /**
     * Perform a foreach operation over the Queues template set and benchmark the queue
     * @note Queues defined in QueueTypeSet.hpp
     * @note if the same queue is contained more than once in the set, the benchmark is executed only one time
     */
   
            std::cout << 
#ifdef DEBUG
            "DEBUG: " <<
#endif
            benchmark(producers,consumers, sizeQueue, duration, center, amplitude,prod_cons) << std::endl;
            
            return 0;
        
}

long double benchmark(size_t producers, size_t consumers, size_t sizeQueue, size_t items,size_t center,size_t amplitude, size_t prod_cons){
    if(producers == 0 || consumers == 0 || sizeQueue == 0 || items == 0){
        std::cerr << "Error: Invalid null arguments" << std::endl;
        exit(1);
    }

    //init variables
    All2All<Data> queue(sizeQueue, producers,consumers); //queue only used by subthreads
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
    sharedArgs.prod_cons = prod_cons;


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
        producer_threads.emplace_back( producer_routine,std::ref(queue), 
                                        producerBatch + (current_tid < remainer ? 1 : 0), //load balance
                                        std::ref(sharedArgs),
                                        current_tid);
        current_tid++;
    }

    //schedule consumers
    while(current_tid < producers + consumers){
        consumer_threads.emplace_back( consumer_routine,std::ref(queue),
                                        std::ref(sharedArgs),
                                        current_tid);
        current_tid++;
    }

    /**
     * if the numa optimization isn't disabled then we
     */
#ifndef DISABLE_AFFINITY
    Dispatcher dispatcher(CACHE_LEVEL);
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
        exit(1);
    }
#endif

    //return the ops per sec
    std::chrono::nanoseconds deltaTime = end - start;
    return static_cast<long double>(items * NSEC_IN_SEC) / deltaTime.count();
}


void producer_routine(All2All<Data> &queue, size_t items, threadShared &sharedArgs, const int tid){
    sharedArgs.threadsBarrier->arrive_and_wait(); //wait for main to set affinity

#ifndef DISABLE_AFFINITY    //if affinity has been set
    (*sharedArgs.threadCluster)[tid] = Dispatcher::get_numa_node();
    sharedArgs.threadsBarrier->arrive_and_wait();
#endif

    Data *item;
#ifdef DEBUG    //get the items to be sent
    items = (*sharedArgs.itemsPerProducer)[tid].size();
#else
    item = new Data();  //no initialization required because it won't be checked
#endif
    const size_t center = sharedArgs.center;
    const size_t amplitude = sharedArgs.amplitude;
    const size_t producers = sharedArgs.producers;
    sharedArgs.threadsBarrier->arrive_and_wait();
    for(size_t i = 0; i < items; i++){
        if(sharedArgs.prod_cons != 2)
            random_work(center,amplitude);   //simulate random work [between minWait and maxWait]
                
#ifdef DEBUG    //use the item from the preallocated vector
        item = &((*(sharedArgs.itemsPerProducer))[tid][i]);
#endif
        //Bounded Queue push
            while(!(queue.push(item,tid))); //push until successful
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


void consumer_routine(All2All<Data> &queue, threadShared &sharedArgs, const int tid){
    sharedArgs.threadsBarrier->arrive_and_wait(); //wait for main to set affinity

#ifndef DISABLE_AFFINITY    //check with main that pinning was successful;
    (*sharedArgs.threadCluster)[tid] = Dispatcher::get_numa_node();
    sharedArgs.threadsBarrier->arrive_and_wait();
#endif

#ifdef DEBUG    //if DEBUG then we check for total number of transfers and FIFO ordering amongst producers
    std::vector<size_t> lastSeen(sharedArgs.producers,0);   //last seen value for each producer
    uint64_t transfer = 0;  //number of items transferred
#endif

    Data *item = nullptr;
    const size_t center = sharedArgs.center;
    const size_t amplitude = sharedArgs.amplitude;
    const size_t consumers = sharedArgs.consumers;
    sharedArgs.threadsBarrier->arrive_and_wait();
    
    while(!sharedArgs.stopFlag->load(std::memory_order_relaxed)){   //the flag value is updated with store(std::memory_order_release)
        item = queue.pop(tid);
        //pop with delay if unsuccessful

#ifdef DEBUG    //the item gets checked if not nullptr
        if(item != nullptr){
            ++transfer;
            consumer_check(lastSeen,item);  //bound to fail if item out of order [else updates lastSeen]
        }
#endif
        if(sharedArgs.prod_cons != 1 && item != nullptr) //RandomWork is done only if the item is extracted
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
        if(sharedArgs.prod_cons != 1)
            random_work(center,amplitude);   //simulate random work [between minWait and maxWait]
    }while(true);

    sharedArgs.threadsBarrier->arrive_and_wait();  //notify main that consumers are done [stops the measure]

#ifdef DEBUG    //set the transfer value before exiting
    (*sharedArgs.itemsPerConsumer)[tid % sharedArgs.consumers] = transfer;  //guarantees mod access to vector
#endif
    return;
}