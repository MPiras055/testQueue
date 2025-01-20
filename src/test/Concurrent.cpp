#include "gtest/gtest.h"
#include <algorithm>
#include <barrier>
#include <vector>
#include <numeric>

#include "QueueTypeSet.hpp"
#include "ThreadGroup.hpp"

#define CONCURRENT_RUN 2


// Define type aliases for each queue type you want to test
template<typename V>
using UQueues = ::testing::Types<FAAQueue<V>,LCRQueue<V>, LPRQueue<V>, LinkedMuxQueue<V>,LMTQueue<V>>;
template<typename V>
using BQueues = ::testing::Types<BoundedMTQueue<V>,BoundedPRQueue<V>,BoundedMuxQueue<V>,BoundedCRQueue<V>>;
//using BoundedQueues = ::testing::Types<BoundedMTQueue<V>>;

// Test setup for unbounded queues
template <typename Q>
class Unbounded_Traits : public ::testing::Test {
public:
    static constexpr int THREADS = 128;
    static constexpr size_t SIZE = 20;
    Q queue;

    // Default constructor with parameters for different queue types
    Unbounded_Traits() : queue(SIZE,THREADS){}
};

struct Data {
    int tid;
    size_t value;
    Data() = default;   //Default Constructor to perform delete operation
    Data(int tid,size_t value) : tid(tid), value(value){}

    auto operator <=> (const Data&) const = default;
};

using UQueuesOfData = UQueues<Data>;
using BQueuesOfData = BQueues<Data>;

// Test setup for unbounded queues
template <typename Q>
class Unbounded : public ::testing::Test {
public:
    static constexpr size_t SIZE = 1024;
    static constexpr int THREADS = 128;
    const size_t RUNS = 5;
    const size_t THREADS_RUN = 2;
    const size_t ITER_ITEMS = 1'000'000;

    Q queue;

    // Default constructor with parameters for different queue types
    Unbounded() : queue(SIZE,THREADS){}
};

//test setup for bounded queues
template <typename Q>
class Bounded : public ::testing::Test {
public:
    static constexpr size_t SIZE = 1024;
    const size_t RUNS = 5;
    const size_t THREADS_RUN     = 4;
    const size_t ITER_ITEMS      = 10000'000;

    Q queue;

    // Default constructor with parameters for different queue types
    Bounded() : queue(SIZE){}
};


TYPED_TEST_SUITE(Unbounded, UQueuesOfData);
TYPED_TEST_SUITE(Bounded, BQueuesOfData);

/**
 * @brief Test if the queue correctly transfers n elements
 * 
 * Schedule 2 groups of threads, one group push an unique digit. Consumers sum the digits 
 * and check if the sum is correct (item*(item+1))*producers/2
 * 
 * @note the test repeats for different numbers of threads [1-1,1-n,n-1,n-n]
 * @note the test repeats each thread configuration for a specified run_count [default: 5]
 */
TYPED_TEST(Unbounded,TransferAllItems){
    std::atomic<bool> stopFlag{false};
    const int runs = this->RUNS;
    const int ItemsPerThread = this->ITER_ITEMS;
    const int threads_per_run = this->THREADS_RUN;
    std::unique_ptr<std::barrier<>> prodBarrier;
    std::unique_ptr<std::barrier<>> threadBarrier;
    ThreadGroup threads;
    const auto producer = [this,ItemsPerThread,&prodBarrier,&threadBarrier](const int tid){
            Data * items = new Data[ItemsPerThread];
            threadBarrier->arrive_and_wait();
            for(size_t i = 0; i< ItemsPerThread; i++){
                items[i] = Data(tid,i+1);
                this->queue.push(&(items[i]),tid);
            }
            prodBarrier->arrive_and_wait();
            threadBarrier->arrive_and_wait();
            delete[] items;
            return;
        };

        const auto consumer = [this,&stopFlag,&threadBarrier](const int tid){
            uint64_t sum = 0;
            Data* item;
            threadBarrier->arrive_and_wait();
            while(!stopFlag.load()){
                item = this->queue.pop(tid);
                if(item != nullptr) sum += item->value;
            }
            while(true){    //Empty the queue after stopFlag is set
                item = this->queue.pop(tid);
                if(item == nullptr) break;
                sum += item->value;
            }
            threadBarrier->arrive_and_wait();  //so producer can safely dealloc dynamic memory
            return sum;
        };



    for(size_t iRun = 0 ; iRun < runs; iRun++){
        std::vector<int> threadsPerRun = {1,threads_per_run};
        for(size_t iProd = 0; iProd < threadsPerRun.size(); iProd++){
            for(size_t iCons = 0; iCons < threadsPerRun.size() ; iCons++){
                const int producers = threadsPerRun[iProd];
                const int consumers = threadsPerRun[iCons];
                std::vector<uint64_t> sum(consumers,0);

                //Init barriers and flag
                prodBarrier = std::make_unique<std::barrier<>>(producers + 1);
                threadBarrier = std::make_unique<std::barrier<>>(producers + consumers + 1);
                stopFlag.store(false);

                //Create threads
                for(int jProd = 0; jProd < producers ; jProd++)
                    threads.thread(producer);
                
                for(int jCons = 0; jCons < consumers ; jCons++)
                    threads.threadWithResult(consumer,sum[jCons]);

                //Start threads
                threadBarrier->arrive_and_wait();
                prodBarrier->arrive_and_wait();
                stopFlag.store(true);
                threadBarrier->arrive_and_wait();
                threads.join();

                //Check if all items have been dequeued
                uint64_t total = std::accumulate(sum.begin(),sum.end(),0);
                total /= producers;

                EXPECT_EQ(total,ItemsPerThread*(ItemsPerThread+1)/2)   << "Failed at run " << iRun 
                                                    << "Got " << total << "Expected " 
                                                    << ItemsPerThread*(ItemsPerThread+1)/2;
                EXPECT_EQ(this->queue.pop(0),nullptr); 
            }
        }
    }
}

/**
 * @brief Same thing as above [retry if push operation unsuccessful]
 */
TYPED_TEST(Bounded,TransferAllItems){
    std::unique_ptr<std::barrier<>> prodBarrier;
    std::unique_ptr<std::barrier<>> threadBarrier;
    std::atomic<bool> stopFlag{false};
    const int runs = this->RUNS;
    const int ItemsPerThread = this->ITER_ITEMS;
    const int threadsRun = this->THREADS_RUN;
    ThreadGroup threads;

    const auto prod_lambda = [this,ItemsPerThread,&prodBarrier,&threadBarrier](const int tid){
        Data * items = new Data[ItemsPerThread];
        threadBarrier->arrive_and_wait();
        for(int i = 0; i< ItemsPerThread; i++){
            items[i] = Data(tid,i+1);
            while(!this->queue.push(&(items[i]),tid)){}
            
        }
        prodBarrier->arrive_and_wait();
        threadBarrier->arrive_and_wait();
        delete[] items;
        return;
    };
    const auto cons_lambda = [this,&stopFlag,&threadBarrier](const int tid){
        Data* item;
        uint64_t sum = 0;
        threadBarrier->arrive_and_wait();
        while(!stopFlag.load()){
            if((item = this->queue.pop(tid)) != nullptr) sum += item->value;
        }
        do{
            if((item = this->queue.pop(tid)) != nullptr) sum += item->value;
        }while(item != nullptr);
        threadBarrier->arrive_and_wait();
        return sum;
    };


    for(int iRun = 0; iRun < runs ; iRun++){
        std::vector<int> threadsPair = {1,threadsRun};
        for(size_t iProd = 0; iProd < threadsPair.size() ; iProd++){
            for(size_t iCons = 0; iCons < threadsPair.size() ; iCons++){
                int producers = threadsPair[iProd];
                int consumers = threadsPair[iCons];
                std::vector<uint64_t> sum(consumers,0);
                //Init barrier
                prodBarrier = std::make_unique<std::barrier<>>(producers + 1);
                threadBarrier = std::make_unique<std::barrier<>>(producers + consumers + 1);
                stopFlag.store(false);

                //Schedule Threads
                for(int jProd = 0; jProd < producers ; jProd++)
                    threads.thread(prod_lambda);
                
                for(int jCons = 0; jCons < consumers ; jCons++)
                    threads.threadWithResult(cons_lambda,sum[jCons]);

                //Start threads
                //std::cout << "Starting threads: " << producers << " producers " << consumers << " consumers" << ItemsPerThread << std::endl;
                threadBarrier->arrive_and_wait();
                prodBarrier->arrive_and_wait();
                stopFlag.store(true);
                threadBarrier->arrive_and_wait();
                threads.join();

                //Check if all items have been dequeued
                uint64_t total = std::accumulate(sum.begin(),sum.end(),0);
                total /= producers;
                ASSERT_EQ(total,ItemsPerThread*(ItemsPerThread+1)/2)   << "Failed at run " << iRun 
                                                    << "Got " << total << "Expected " 
                                                    << ItemsPerThread*(ItemsPerThread+1)/2;
                ASSERT_EQ(this->queue.pop(0),nullptr);
            }
        }
    }
}

/**
 *  @brief Check if the queue semantics is respected during extraction operations
 * 
 * Schedule 2 groups of threads that push and pop the same amount of elements on and off the queue
 * Then check if the elements are correctly dequeued (for each thread older elements should be dequeued before newer elements)
 * 
 * Then add the elements to 2 multisets and check if the elements are exactely the same
 * 
 * @note the test repeats for different numbers of threads [1-1,1-n,n-1,n-n]
 * @note the test repeats each thread configuration for a specified run_count [default: 5]
 */
TYPED_TEST(Unbounded, QueueSemantics) {
    const int numRuns = this->RUNS;
    const int ItemsPerThread = this->ITER_ITEMS;
    const int threadsRun = this->THREADS_RUN;
    std::atomic<bool> stopFlag{false};  
    std::unique_ptr<std::barrier<>> prodBarrier;
    std::unique_ptr<std::barrier<>> threadBarrier;
    std::vector<std::vector<Data>> ItemsToInsert;
    std::vector<std::vector<Data>> ItemsToExtract;  
    ThreadGroup threads;

    const auto prod_lambda = [this,&ItemsToInsert ,&prodBarrier, &threadBarrier](const int vectorIdx,const int tid) {
        threadBarrier->arrive_and_wait();
        for (Data& item : ItemsToInsert[vectorIdx]) {
            this->queue.push(&item, tid);
        }
        prodBarrier->arrive_and_wait();
    };

    const auto cons_lambda = [this, &stopFlag,&ItemsToExtract, &threadBarrier](const int vectorIdx,const int tid) {
        Data* item;
        threadBarrier->arrive_and_wait();
        while (!stopFlag.load()) {
            item = this->queue.pop(tid);
            if (item != nullptr) ItemsToExtract[vectorIdx].push_back(*item);
        }
        do {
            item = this->queue.pop(tid);
            if (item != nullptr) ItemsToExtract[vectorIdx].push_back(*item);
        } while (item != nullptr);
    };

    for (int iRun = 0; iRun < numRuns; iRun++) {
        std::vector<int> threadsPair = {1, threadsRun};
        for (size_t iProd = 0; iProd < threadsPair.size(); iProd++) {
            for (size_t iCons = 0; iCons < threadsPair.size(); iCons++) {
                const int producers = threadsPair[iProd];
                const int consumers = threadsPair[iCons];
                ItemsToInsert = std::vector<std::vector<Data>>(producers);
                ItemsToExtract = std::vector<std::vector<Data>>(consumers);
                
                // Ensure proper initialization of ItemsToInsert and ItemsToExtract

                // Initialize producer Matrix
                for (int iMatrix = 0; iMatrix < producers; iMatrix++) {
                    ItemsToInsert.push_back(std::vector<Data>());
                    for (int iData = 0; iData < ItemsPerThread; iData++) {
                        ItemsToInsert[iMatrix].push_back(Data(iMatrix, iData));
                    }
                }

                // Initialize consumer Matrix
                for (int iMatrix = 0; iMatrix < consumers; iMatrix++) {
                    ItemsToExtract.push_back(std::vector<Data>());
                }


                // Initialize barrier
                prodBarrier   = std::make_unique<std::barrier<>>(producers + 1);
                threadBarrier = std::make_unique<std::barrier<>>(producers + consumers + 1);
                stopFlag.store(false);

                // Start threads
                for (int jProd = 0; jProd < producers; jProd++) {
                    threads.thread(prod_lambda,jProd);
                }
                for (int jCons = 0; jCons < consumers; jCons++) {
                    threads.thread(cons_lambda,jCons);
                }

                threadBarrier->arrive_and_wait();
                prodBarrier->arrive_and_wait();
                stopFlag.store(true);
                threads.join();

                // Validate consumer data
                for (std::vector<Data>& consVector : ItemsToExtract) {
                    std::stable_sort(std::begin(consVector), std::end(consVector), [](const Data& a, const Data& b) { 
                        return a.tid < b.tid; 
                    });
                    for (size_t iItem = 1; iItem < consVector.size(); iItem++) {
                        const Data& deq1 = consVector[iItem - 1];
                        const Data& deq2 = consVector[iItem];
                        if (deq1.tid == deq2.tid) {
                            EXPECT_LT(deq1.value, deq2.value) << "Failed at run " << iRun;
                        }
                    }
                }

                std::multiset<Data> producerItems, consumerItems;
                for (const std::vector<Data>& data : ItemsToInsert) {
                    producerItems.insert(data.begin(), data.end());
                }

                std::multiset<Data> consJoined;
                for (const std::vector<Data>& data : ItemsToExtract) {
                    consumerItems.insert(data.begin(), data.end());
                }

                ASSERT_EQ(producerItems.size(), consumerItems.size());
                ASSERT_EQ(producerItems,consumerItems);

                stopFlag.store(false);
            }
        }
    }
}

/**
 * @brief same as above [retry if push fails]
 */
TYPED_TEST(Bounded, QueueSemantics) {
    const int numRuns = this->RUNS;
    const int ItemsPerThread = this->ITER_ITEMS;
    const int threadsRun = this->THREADS_RUN;
    std::atomic<bool> stopFlag{false};  
    std::unique_ptr<std::barrier<>> prodBarrier;
    std::unique_ptr<std::barrier<>> threadBarrier;
    std::vector<std::vector<Data>> ItemsToInsert;
    std::vector<std::vector<Data>> ItemsToExtract;  
    ThreadGroup threads;

    const auto prod_lambda = [this,&ItemsToInsert ,&prodBarrier, &threadBarrier](const int vectorIdx,const int tid) {
        threadBarrier->arrive_and_wait();
        for (Data& item : ItemsToInsert[vectorIdx]) {
            while(this->queue.push(&item, tid) == false){};
        }
        prodBarrier->arrive_and_wait();
    };

    const auto cons_lambda = [this, &stopFlag,&ItemsToExtract, &threadBarrier](const int vectorIdx,const int tid) {
        Data* item;
        threadBarrier->arrive_and_wait();
        while (!stopFlag.load()) {
            item = this->queue.pop(tid);
            if (item != nullptr) ItemsToExtract[vectorIdx].push_back(*item);
        }
        do {
            item = this->queue.pop(tid);
            if (item != nullptr) ItemsToExtract[vectorIdx].push_back(*item);
        } while (item != nullptr);
    };

    for (int iRun = 0; iRun < numRuns; iRun++) {
        std::vector<int> threadsPair = {1, threadsRun};
        for (size_t iProd = 0; iProd < threadsPair.size(); iProd++) {
            for (size_t iCons = 0; iCons < threadsPair.size(); iCons++) {
                const int producers = threadsPair[iProd];
                const int consumers = threadsPair[iCons];
                ItemsToInsert = std::vector<std::vector<Data>>(producers);
                ItemsToExtract = std::vector<std::vector<Data>>(consumers);
                
                // Ensure proper initialization of ItemsToInsert and ItemsToExtract

                // Initialize producer Matrix
                for (int iMatrix = 0; iMatrix < producers; iMatrix++) {
                    ItemsToInsert.push_back(std::vector<Data>());
                    for (int iData = 0; iData < ItemsPerThread; iData++) {
                        ItemsToInsert[iMatrix].push_back(Data(iMatrix, iData));
                    }
                }

                // Initialize consumer Matrix
                for (int iMatrix = 0; iMatrix < consumers; iMatrix++) {
                    ItemsToExtract.push_back(std::vector<Data>());
                }


                // Initialize barrier
                prodBarrier   = std::make_unique<std::barrier<>>(producers + 1);
                threadBarrier = std::make_unique<std::barrier<>>(producers + consumers + 1);
                stopFlag.store(false);

                // Start threads
                for (int jProd = 0; jProd < producers; jProd++) {
                    threads.thread(prod_lambda,jProd);
                }
                for (int jCons = 0; jCons < consumers; jCons++) {
                    threads.thread(cons_lambda,jCons);
                }

                threadBarrier->arrive_and_wait();
                prodBarrier->arrive_and_wait();
                stopFlag.store(true);
                threads.join();

                // Validate consumer data
                for (std::vector<Data>& consVector : ItemsToExtract) {
                    std::stable_sort(std::begin(consVector), std::end(consVector), [](const Data& a, const Data& b) { 
                        return a.tid < b.tid; 
                    });
                    for (size_t iItem = 1; iItem < consVector.size(); iItem++) {
                        const Data& deq1 = consVector[iItem - 1];
                        const Data& deq2 = consVector[iItem];
                        if (deq1.tid == deq2.tid) {
                            EXPECT_LT(deq1.value, deq2.value) << "Failed at run " << iRun;
                        }
                    }
                }

                std::multiset<Data> producerItems, consumerItems;
                for (const std::vector<Data>& data : ItemsToInsert) {
                    producerItems.insert(data.begin(), data.end());
                }

                std::multiset<Data> consJoined;
                for (const std::vector<Data>& data : ItemsToExtract) {
                    consumerItems.insert(data.begin(), data.end());
                }

                ASSERT_EQ(producerItems.size(), consumerItems.size());
                ASSERT_EQ(producerItems,consumerItems);

                stopFlag.store(false);
            }
        }
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();  // This runs all tests   
}

