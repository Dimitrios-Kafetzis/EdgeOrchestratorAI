/**
 * @file test_thread_pool.cpp
 * @brief Unit tests for ThreadPool.
 */

#include "executor/thread_pool.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>

using namespace edge_orchestrator;

TEST(ThreadPoolTest, BasicSubmit) {
    ThreadPool pool(2);
    auto future = pool.submit([] { return 42; });
    EXPECT_EQ(future.get(), 42);
}

TEST(ThreadPoolTest, MultipleSubmissions) {
    ThreadPool pool(4);
    std::vector<std::future<int>> futures;

    for (size_t i = 0; i < 100; ++i) {
        futures.push_back(pool.submit([i] { return static_cast<int>(i * i); }));
    }

    for (size_t i = 0; i < 100; ++i) {
        EXPECT_EQ(futures[i].get(), static_cast<int>(i * i));
    }
}

TEST(ThreadPoolTest, ConcurrentExecution) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;

    for (int i = 0; i < 100; ++i) {
        futures.push_back(pool.submit([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    for (auto& f : futures) f.get();
    EXPECT_EQ(counter.load(), 100);
}

TEST(ThreadPoolTest, ThreadCount) {
    ThreadPool pool(3);
    EXPECT_EQ(pool.thread_count(), 3u);
}
