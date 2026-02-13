/**
 * @file test_types.cpp
 * @brief Unit tests for core types.
 */

#include "core/types.hpp"

#include <gtest/gtest.h>

using namespace edge_orchestrator;

TEST(ResourceSnapshotTest, MemoryUsagePercent) {
    ResourceSnapshot snap;
    snap.memory_total_bytes = 4ULL * 1024 * 1024 * 1024;  // 4 GB
    snap.memory_available_bytes = 1ULL * 1024 * 1024 * 1024;  // 1 GB

    EXPECT_FLOAT_EQ(snap.memory_usage_percent(), 75.0f);
}

TEST(ResourceSnapshotTest, MemoryUsageZeroTotal) {
    ResourceSnapshot snap;
    snap.memory_total_bytes = 0;
    snap.memory_available_bytes = 0;

    EXPECT_FLOAT_EQ(snap.memory_usage_percent(), 0.0f);
}

TEST(ResourceSnapshotTest, CpuHeadroom) {
    ResourceSnapshot snap;
    snap.cpu_usage_percent = 60.0f;

    EXPECT_FLOAT_EQ(snap.cpu_headroom_percent(), 40.0f);
}

TEST(TaskStateTest, ToString) {
    EXPECT_EQ(to_string(TaskState::Pending), "pending");
    EXPECT_EQ(to_string(TaskState::Completed), "completed");
    EXPECT_EQ(to_string(TaskState::Failed), "failed");
}

TEST(TaskProfileTest, ThreeWayComparison) {
    TaskProfile a{.compute_cost = Duration{100}, .memory_bytes = 1024};
    TaskProfile b{.compute_cost = Duration{200}, .memory_bytes = 1024};

    EXPECT_TRUE(a < b);
    EXPECT_FALSE(b < a);
    EXPECT_TRUE(a == a);
}
