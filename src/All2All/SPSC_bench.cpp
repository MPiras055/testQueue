#include "SPSC.hpp"
#include <vector>
#include <chrono>
#include <barrier>
#include <iostream>
#include "NumaDispatcher.hpp"
#include "AdditionalWork.hpp"

#define CACHE_LEVEL 3
#define NSEC_IN_SEC 1'000'000'000ULL
#define PRINT_CLUSTER false
//Parameters for exponential delay
#define MIN_BACKOFF     128ull //~250ns
#define MAX_BACKOFF     1024ull //~850ns

//#define DEBUG 0 //[uncomment to check queue correctness]
//#define DISABLE_AFFINITY 0 //[uncomment to disable affinity setting]

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

struct threadShared {
    std::barrier<>* threadsBarrier = nullptr;
    std::barrier<>* producersBarrier = nullptr;
    std::barrier<>* consumersBarrier = nullptr;
    std::vector<Data> *itemsPerProducer = nullptr;
    std::atomic<bool> *stopFlag = nullptr;
    std::vector<int>  *threadCluster = nullptr;
    size_t items;
    size_t consumerTotalTransfers = 0;
    threadShared() = default;  
};

void producer_routine(SPSC<Data> &queue, size_t min_wait, size_t max_wait,threadShared &sharedArgs, const int tid);

void consumer_routine(SPSC<Data> &queue, size_t min_wait, size_t max_wait, threadShared &sharedArgs, const int tid);

long double benchmark(size_t sizeQueue, size_t items, size_t min_wait, size_t max_wait);

int main(int argc, char **argv){
    if(argc != 5){
        std::cerr << "Usage: " << argv[0] << " <sizeQueue> <items> <min_wait> <max_wait>" << std::endl;
        exit(1);
    }

    size_t sizeQueue = std::stoul(argv[1]);
    size_t items = std::stoul(argv[2]);
    size_t min_wait = std::stoul(argv[3]);
    size_t max_wait = std::stoul(argv[4]);

    std::cout << 
#ifdef DEBUG
    "DEBUG: " <<
#endif
    benchmark(sizeQueue, items, min_wait, max_wait) <<
    std::endl;

    return 0;
}

long double benchmark(size_t sizeQueue, size_t items, size_t min_wait, size_t max_wait){
    SPSC<Data> queue = SPSC<Data>(sizeQueue);
    std::barrier<> allThreads(3);
    std::barrier<> producersBarrier(2);
    std::barrier<> consumersBarrier(2);
    std::atomic<bool> stopFlag(false);
    threadShared sharedArgs;

    sharedArgs.items = items;
    sharedArgs.threadsBarrier   = &allThreads;
    sharedArgs.producersBarrier = &producersBarrier;
    sharedArgs.consumersBarrier = &consumersBarrier;
    sharedArgs.stopFlag = &stopFlag;

#ifndef DISABLE_AFFINITY
    std::vector<int> threadCluster(2,-1);
    sharedArgs.threadCluster = &threadCluster;
#endif

#ifdef DEBUG
    std::vector<Data> itemsProducer;
    itemsProducer.reserve(items);
    for(size_t i = 0; i < items; i++){
        itemsProducer.push_back(Data(0,i+1));
    }
    sharedArgs.itemsPerProducer = &itemsProducer;
#endif
    //need vectors for numa dispatcher
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;   
    producers.emplace_back( producer_routine,
                            std::ref(queue),
                            min_wait,max_wait,
                            std::ref(sharedArgs),0
                            );
    producers.emplace_back( consumer_routine,
                            std::ref(queue),
                            min_wait,max_wait,
                            std::ref(sharedArgs),1
                            );

#ifndef DISABLE_AFFINITY
    Dispatcher dispatcher(CACHE_LEVEL);
    dispatcher.dispatch_threads(producers,consumers);
#endif

    allThreads.arrive_and_wait();
#ifndef DISABLE_AFFINITY
    allThreads.arrive_and_wait();   //wait for threads to communicate their cluster
    bool clusterSetError = false;
    for(int i = 0; i < threadCluster.size(); i++){
        if(threadCluster[i] < 0){
            std::cerr << "Cluster Error: Thread " << i << "not assigned. Cluster: " << threadCluster[i] << "\n";
            clusterSetError = true;
        } else {
#ifdef DEBUG
#if PRINT_CLUSTER
            std::cout << "Thread " << i << " running on Cluster " << threadCluster[i] << "\n";
#endif
#endif
        }
    }
    std::cout.flush();
    assert(!clusterSetError);
#endif
    allThreads.arrive_and_wait();
    auto start = std::chrono::high_resolution_clock::now();
    producersBarrier.arrive_and_wait();
    stopFlag.store(true,std::memory_order_release);
    allThreads.arrive_and_wait();
    auto end = std::chrono::high_resolution_clock::now();
    //join threads
    producers[0].join();
    producers[1].join();
#ifdef DEBUG
    if(sharedArgs.consumerTotalTransfers != items){
        std::cerr << "ERROR: Consumer didn't receive all items. Expected: " << items << " Received: " << sharedArgs.consumerTotalTransfers << std::endl;
        exit(1);
    }
#endif
    std::chrono::nanoseconds deltaTime = end - start;
    return static_cast<long double>(items * NSEC_IN_SEC) / deltaTime.count();
    return 0;
}

void producer_routine(SPSC<Data> &queue, size_t min_wait, size_t max_wait, threadShared &sharedArgs, const int tid){
    sharedArgs.threadsBarrier->arrive_and_wait();
#ifndef DISABLE_AFFINITY
    (*sharedArgs.threadCluster)[tid] = Dispatcher::get_numa_node();
    sharedArgs.threadsBarrier->arrive_and_wait();
#endif
    size_t items = sharedArgs.items;
    Data *item;
#ifdef DEBUG
    items = (*sharedArgs.itemsPerProducer).size();
#else
    item = new Data();
#endif
    uint64_t delay = MIN_BACKOFF;
    sharedArgs.threadsBarrier->arrive_and_wait();
    for(size_t i = 0; i < items; i++){
        random_work(min_wait,max_wait);
#ifdef DEBUG
        item = &(*sharedArgs.itemsPerProducer)[i];
#endif
        while(!queue.push(item)){
            loop(delay);
            delay <<= 1;
            delay = ((delay - 1) & (MAX_BACKOFF - 1)) + 1; //clamped bound
        }
        delay = MIN_BACKOFF;
    }
#ifdef MULTIPUSH
    uint failed = 0;
    //flush the last batch of items
    while(!queue.flush()){
        volatile bool dummy = false;
        for(uint i = 0; i < randint(MIN_BACKOFF) || dummy; i++){}
        if(failed == MAX_FAIL_PUSH){
            std::this_thread::sleep_for(std::chrono::microseconds(1));
            failed = 0;
        }
        failed++;
    }
#endif

    sharedArgs.producersBarrier->arrive_and_wait();
    sharedArgs.threadsBarrier->arrive_and_wait();
#ifndef DEBUG
    delete item;
#endif
    return;
}

void consumer_routine(SPSC<Data> &queue, size_t min_wait, size_t max_wait, threadShared &sharedArgs, const int tid){
    sharedArgs.threadsBarrier->arrive_and_wait();
#ifndef DISABLE_AFFINITY
    (*sharedArgs.threadCluster)[tid] = Dispatcher::get_numa_node();
    sharedArgs.threadsBarrier->arrive_and_wait();
#endif
#ifdef DEBUG
    size_t lastSeen = 0; //last value seen from producer
    size_t totalTransfers = 0;
#endif
    uint64_t delay = MIN_BACKOFF;
    Data *item = nullptr;
    sharedArgs.threadsBarrier->arrive_and_wait();
    while(!sharedArgs.stopFlag->load(std::memory_order_acquire)){
        random_work(min_wait,max_wait);
        item = queue.pop();
        if(item == nullptr){    //delay
            loop(delay);
            delay <<= 1;
            delay = ((delay - 1) & (MAX_BACKOFF - 1)) + 1; //clamped bound
            continue;
        } else delay = MIN_BACKOFF;
#ifdef DEBUG
        if(item != nullptr){
            ++totalTransfers;
            if(lastSeen >= item->value){
                std::cerr << "ERROR: Producer " << item->tid << " sent item " << item->value << " after " << lastSeen << std::endl;
                exit(1);
            }
        }
#endif
    }
    //Drain the queue
    do{
        item = queue.pop();
#ifdef DEBUG
        if(item != nullptr){
            ++totalTransfers;
            if(lastSeen >= item->value){
                std::cerr << "ERROR: Producer " << item->tid << " sent item " << item->value << " after " << lastSeen << std::endl;
                exit(1);
            }
        }
#endif
    }while(item != nullptr);
    sharedArgs.threadsBarrier->arrive_and_wait();
#ifdef DEBUG
    sharedArgs.consumerTotalTransfers = totalTransfers;
#endif
    return;
}

