/**
 * @file test_mpmc_queue.cpp
 * @brief Unit and stress tests for the lock-free MPMC queue.
 * @author Dimitris Kafetzis
 */

#include "executor/mpmc_queue.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <numeric>
#include <thread>
#include <vector>

using namespace edge_orchestrator;

TEST(MpmcQueueTest, PushPopSingleThread) {
    MpmcQueue<int> q(8);
    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(q.try_push(int{i}));
    }
    int out;
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out, i);  // FIFO
    }
    EXPECT_FALSE(q.try_pop(out));
}

TEST(MpmcQueueTest, RejectsWhenFull) {
    MpmcQueue<int> q(4);
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.try_push(int{i}));
    }
    EXPECT_FALSE(q.try_push(99));

    int out;
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_TRUE(q.try_push(99));  // slot freed, usable again
}

TEST(MpmcQueueTest, CapacityRoundsUpToPowerOfTwo) {
    MpmcQueue<int> q(5);
    EXPECT_EQ(q.capacity(), 8u);
}

TEST(MpmcQueueTest, WrapsAroundManyLaps) {
    MpmcQueue<int> q(4);
    int out;
    for (int lap = 0; lap < 1000; ++lap) {
        ASSERT_TRUE(q.try_push(int{lap}));
        ASSERT_TRUE(q.try_pop(out));
        ASSERT_EQ(out, lap);
    }
}

// The property that matters: with 4 producers and 4 consumers hammering
// the queue, nothing is lost, duplicated, or torn.
TEST(MpmcQueueTest, ConcurrentProducersConsumersPreserveSum) {
    constexpr int PRODUCERS = 4;
    constexpr int CONSUMERS = 4;
    constexpr int PER_PRODUCER = 25000;

    MpmcQueue<int> q(256);
    std::atomic<long long> consumed_sum{0};
    std::atomic<int> consumed_count{0};

    std::vector<std::jthread> threads;
    for (int c = 0; c < CONSUMERS; ++c) {
        threads.emplace_back([&]() {
            int out;
            while (consumed_count.load() < PRODUCERS * PER_PRODUCER) {
                if (q.try_pop(out)) {
                    consumed_sum.fetch_add(out);
                    consumed_count.fetch_add(1);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (int p = 0; p < PRODUCERS; ++p) {
        threads.emplace_back([&, p]() {
            for (int i = 0; i < PER_PRODUCER; ++i) {
                int value = p * PER_PRODUCER + i;
                while (!q.try_push(std::move(value))) {
                    value = p * PER_PRODUCER + i;  // restore after move
                    std::this_thread::yield();
                }
            }
        });
    }
    threads.clear();  // join all

    long long n = static_cast<long long>(PRODUCERS) * PER_PRODUCER;
    EXPECT_EQ(consumed_count.load(), n);
    EXPECT_EQ(consumed_sum.load(), n * (n - 1) / 2);
}
