/**
 * @file test_memory_pool.cpp
 * @brief Unit tests for MemoryPool arena allocator.
 */

#include "executor/memory_pool.hpp"

#include <gtest/gtest.h>

using namespace edge_orchestrator;

TEST(MemoryPoolTest, BasicAllocation) {
    MemoryPool pool(1024);
    void* ptr = pool.allocate(256);
    ASSERT_NE(ptr, nullptr);
    EXPECT_GE(pool.used(), 256u);
}

TEST(MemoryPoolTest, Reset) {
    MemoryPool pool(1024);
    auto* ptr = pool.allocate(512);
    (void)ptr;  // value not needed, testing used() metric
    EXPECT_GE(pool.used(), 512u);

    pool.reset();
    EXPECT_EQ(pool.used(), 0u);
}

TEST(MemoryPoolTest, ExceedsCapacity) {
    MemoryPool pool(256);
    void* ptr = pool.allocate(512);
    EXPECT_EQ(ptr, nullptr);
}

TEST(MemoryPoolTest, MultipleAllocations) {
    MemoryPool pool(4096);
    void* a = pool.allocate(100);
    void* b = pool.allocate(100);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b);
}

TEST(MemoryPoolTest, CanAllocateCheck) {
    MemoryPool pool(256);
    EXPECT_TRUE(pool.can_allocate(128));
    EXPECT_FALSE(pool.can_allocate(512));
}

TEST(MemoryPoolTest, Capacity) {
    MemoryPool pool(8192);
    EXPECT_EQ(pool.capacity(), 8192u);
}
