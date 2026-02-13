/**
 * @file test_monitor.cpp
 * @brief Unit tests for MockMonitor and LinuxMonitor.
 * @author Dimitris Kafetzis
 */

#include "resource_monitor/monitor.hpp"

#include <gtest/gtest.h>
#include <chrono>

using namespace edge_orchestrator;

// ─── MockMonitor ─────────────────────────────

TEST(MockMonitorTest, DefaultSnapshot) {
    MockMonitor monitor("test-node");
    monitor.start();

    auto result = monitor.read();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->node_id, "test-node");
    EXPECT_FLOAT_EQ(result->cpu_usage_percent, 25.0f);
    EXPECT_EQ(result->memory_total_bytes, 4ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(result->memory_available_bytes, 2ULL * 1024 * 1024 * 1024);
    EXPECT_FALSE(result->is_throttled);
}

TEST(MockMonitorTest, SetCpu) {
    MockMonitor monitor("node-1");
    monitor.set_cpu(85.5f);

    auto result = monitor.read();
    ASSERT_TRUE(result.has_value());
    EXPECT_FLOAT_EQ(result->cpu_usage_percent, 85.5f);
    EXPECT_FLOAT_EQ(monitor.cpu_usage(), 85.5f);
}

TEST(MockMonitorTest, SetMemory) {
    MockMonitor monitor("node-1");
    uint64_t avail = 1ULL * 1024 * 1024 * 1024;  // 1 GB
    uint64_t total = 8ULL * 1024 * 1024 * 1024;   // 8 GB
    monitor.set_memory(avail, total);

    auto result = monitor.read();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->memory_available_bytes, avail);
    EXPECT_EQ(result->memory_total_bytes, total);
    EXPECT_FLOAT_EQ(result->memory_usage_percent(), 87.5f);
    EXPECT_EQ(monitor.memory_available(), avail);
}

TEST(MockMonitorTest, SetThrottled) {
    MockMonitor monitor("node-1");
    EXPECT_FALSE(monitor.is_throttled());
    monitor.set_throttled(true);
    EXPECT_TRUE(monitor.is_throttled());
}

TEST(MockMonitorTest, SequenceMode) {
    MockMonitor monitor("node-1");

    ResourceSnapshot snap1;
    snap1.cpu_usage_percent = 10.0f;
    snap1.memory_available_bytes = 3ULL * 1024 * 1024 * 1024;
    snap1.memory_total_bytes = 4ULL * 1024 * 1024 * 1024;

    ResourceSnapshot snap2;
    snap2.cpu_usage_percent = 90.0f;
    snap2.memory_available_bytes = 512 * 1024 * 1024;
    snap2.memory_total_bytes = 4ULL * 1024 * 1024 * 1024;

    monitor.push_snapshot(snap1);
    monitor.push_snapshot(snap2);

    auto r1 = monitor.read();
    ASSERT_TRUE(r1.has_value());
    EXPECT_FLOAT_EQ(r1->cpu_usage_percent, 10.0f);
    EXPECT_EQ(r1->node_id, "node-1");  // node_id forced to monitor's

    auto r2 = monitor.read();
    ASSERT_TRUE(r2.has_value());
    EXPECT_FLOAT_EQ(r2->cpu_usage_percent, 90.0f);

    // Sequence exhausted
    auto r3 = monitor.read();
    EXPECT_FALSE(r3.has_value());
}

TEST(MockMonitorTest, SetStaticAfterSequence) {
    MockMonitor monitor("node-1");

    ResourceSnapshot snap;
    snap.cpu_usage_percent = 50.0f;
    monitor.push_snapshot(snap);
    monitor.read();  // consume sequence

    // Switch back to static
    ResourceSnapshot static_snap;
    static_snap.cpu_usage_percent = 30.0f;
    static_snap.memory_total_bytes = 4ULL * 1024 * 1024 * 1024;
    static_snap.memory_available_bytes = 2ULL * 1024 * 1024 * 1024;
    monitor.set_static_snapshot(static_snap);

    auto result = monitor.read();
    ASSERT_TRUE(result.has_value());
    EXPECT_FLOAT_EQ(result->cpu_usage_percent, 30.0f);
}

TEST(MockMonitorTest, TimestampIsRecent) {
    MockMonitor monitor("node-1");
    auto before = std::chrono::system_clock::now();
    auto result = monitor.read();
    auto after = std::chrono::system_clock::now();

    ASSERT_TRUE(result.has_value());
    EXPECT_GE(result->timestamp, before);
    EXPECT_LE(result->timestamp, after);
}

// ─── LinuxMonitor (can test on any Linux host) ───

TEST(LinuxMonitorTest, Construct) {
    LinuxMonitor monitor("linux-node", 500);
    // Should not crash
}

TEST(LinuxMonitorTest, ReadBeforeStart) {
    LinuxMonitor monitor("linux-node", 500);
    auto result = monitor.read();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "No snapshot available yet");
}

TEST(LinuxMonitorTest, StartAndRead) {
    LinuxMonitor monitor("linux-node", 100);
    monitor.start();

    // Give the sampling thread time to produce a snapshot
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    auto result = monitor.read();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->node_id, "linux-node");

    // On any Linux host these should be non-zero
    EXPECT_GT(result->memory_total_bytes, 0u);
    EXPECT_GT(result->memory_available_bytes, 0u);

    monitor.stop();
}

TEST(LinuxMonitorTest, CpuUsageRange) {
    LinuxMonitor monitor("linux-node", 100);
    monitor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    auto result = monitor.read();
    ASSERT_TRUE(result.has_value());
    // CPU usage should be between 0 and 100
    EXPECT_GE(result->cpu_usage_percent, 0.0f);
    EXPECT_LE(result->cpu_usage_percent, 100.0f);

    monitor.stop();
}

TEST(LinuxMonitorTest, MemoryComputations) {
    LinuxMonitor monitor("linux-node", 100);
    monitor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    auto result = monitor.read();
    ASSERT_TRUE(result.has_value());

    EXPECT_LE(result->memory_available_bytes, result->memory_total_bytes);
    EXPECT_GE(result->memory_usage_percent(), 0.0f);
    EXPECT_LE(result->memory_usage_percent(), 100.0f);
    EXPECT_FLOAT_EQ(result->cpu_headroom_percent(), 100.0f - result->cpu_usage_percent);

    monitor.stop();
}

TEST(LinuxMonitorTest, ThresholdCallback) {
    LinuxMonitor monitor("linux-node", 100);
    bool cpu_triggered = false;

    // Set a very low threshold so it's guaranteed to trigger
    monitor.on_cpu_threshold(0.0f, [&](const ResourceSnapshot&) {
        cpu_triggered = true;
    });

    monitor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    monitor.stop();

    EXPECT_TRUE(cpu_triggered);
}
