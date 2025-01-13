#include "gtest/gtest.h"

#include "QueueTypeSet.hpp"
#include "ThreadGroup.hpp"

/**
 * Type aliases for queue definition
 */
template<typename V>
using UQueues = ::testing::Types<FAAQueue<V>,LCRQueue<V>, LPRQueue<V>, LinkedMuxQueue<V>,LMTQueue<V>>;
template<typename V>
using BQueues = ::testing::Types<BoundedMTQueue<V>,BoundedPRQueue<V>,BoundedMuxQueue<V>,BoundedCRQueue<V>>;

template<typename Q>
class UnboundedSequential : public ::testing::Test {
protected:
    Q queue;
public:
    static constexpr int THREADS = 128;
    static constexpr int SIZE = 20;
    UnboundedSequential() : queue(SIZE,THREADS){}
    ~UnboundedSequential() = default;
};

template<typename Q>
class BoundedSequential : public ::testing::Test {
protected:
    Q queue;
public:
    static constexpr int SIZE = 20;
    BoundedSequential() :queue(SIZE){}
    ~BoundedSequential() = default;
};


TYPED_TEST_SUITE(UnboundedSequential,UQueues<int>);
TYPED_TEST_SUITE(BoundedSequential,BQueues<int>);

/**
 * @brief Checks if the queue is initialized correctly as empty and with length 0
 */
TYPED_TEST(UnboundedSequential,Initialization){
    int tid = 0;
    EXPECT_EQ(this->queue.length(tid),0);
    for(size_t i = 0; i< this->queue.capacity() * 2; ++i){
        EXPECT_EQ(this->queue.pop(tid),nullptr);
    }
}
TYPED_TEST(BoundedSequential,Initialization){
    int tid = 0;
    EXPECT_EQ(this->queue.length(tid),0);
    for(size_t i = 0; i< this->queue.capacity() * 2; ++i){
        EXPECT_EQ(this->queue.pop(tid),nullptr);
    }
}

/**
 * @brief Tests push and pop operations in sequence
 * 
 * @note the queue is empty at the beginning
 */
TYPED_TEST(UnboundedSequential,EnqueueDequeue){
    int tid = 0;
    size_t length = this->queue.capacity() * 2;
    std::vector<int> item(length);
    size_t Operations = this->queue.capacity() * 5;

    for(size_t i = 0; i < Operations; i++){
        this->queue.push(&item[i % length],tid);
        EXPECT_EQ(this->queue.pop(tid),&item[i % length]);
    }
}
TYPED_TEST(BoundedSequential,EnqueueDequeue){
    int tid = 0;
    size_t length = this->queue.capacity() * 2;
    std::vector<int> item(length);
    size_t Operations = this->queue.capacity() * 5;

    for(size_t i = 0; i < Operations; i++){
        this->queue.push(&item[i % length],tid);
        EXPECT_EQ(this->queue.pop(tid),&item[i % length]);
    }
}

/**
 * @brief Tests overflowRingQueue
 * 
 * @note the queue is empty at the beginning
 * @note if queue is bounded check if pop fails
 * @note if queue is unbounded check if the pop is successful
 */

TYPED_TEST(UnboundedSequential,Overflow){
    size_t length = this->queue.capacity() * 2;
    std::vector<int> item(length);
    size_t Operations = this->queue.capacity() * 5;
    int tid = 0;

    for(size_t i = 0; i < Operations; i++){
        this->queue.push(&item[i % length],tid);
    }
    for(size_t i = 0; i < Operations; i++){
        EXPECT_EQ(this->queue.pop(tid),&item[i % length]);
    }
}

TYPED_TEST(BoundedSequential,Overflow){
    size_t length = this->queue.capacity(); 
    std::vector<int> item(length);
    size_t Operations = length * 5;
    int tid = 0;

    for(size_t i = 0; i < length; i++){
        EXPECT_EQ(this->queue.push(&item[i],tid),true);
    }

    for(size_t i = length; i < Operations; i++){
        EXPECT_EQ(this->queue.push(&item[i],tid),false);
    }

    for(size_t i = 0; i < length; i++){
        EXPECT_EQ(this->queue.pop(tid),&(item[i % length]));
    }

    for(size_t i = length; i < Operations; i++){
        EXPECT_EQ(this->queue.pop(tid),nullptr);
    }

}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();  // This runs all tests   
}
