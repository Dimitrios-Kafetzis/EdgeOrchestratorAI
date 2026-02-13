/**
 * @file monitor.hpp
 * @brief Resource monitor interface and concrete implementations.
 * @author Dimitris Kafetzis
 *
 * Provides LinuxMonitor (reads from /proc, /sys) and MockMonitor (testing).
 * Both satisfy the ResourceMonitorLike concept for zero-cost static dispatch.
 */

#pragma once

#include "core/concepts.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace edge_orchestrator {

/**
 * @brief Callback invoked when a resource threshold is crossed.
 */
using ThresholdCallback = std::function<void(const ResourceSnapshot&)>;

// ─────────────────────────────────────────────
// LinuxMonitor
// ─────────────────────────────────────────────

/**
 * @brief Reads system resources from Linux pseudo-filesystems.
 *
 * Satisfies ResourceMonitorLike. Runs a dedicated sampling thread
 * (std::jthread) and stores the latest snapshot atomically.
 *
 * Data sources:
 *   /proc/stat          — CPU utilization (per-core and aggregate)
 *   /proc/meminfo       — Memory total and available
 *   /sys/class/thermal/ — CPU and GPU temperature
 *   /proc/net/dev       — Network interface byte counters
 *   soc:firmware        — RPi thermal throttle status
 */
class LinuxMonitor {
public:
    explicit LinuxMonitor(NodeId node_id, uint32_t sampling_interval_ms = 500);
    ~LinuxMonitor();

    // Non-copyable
    LinuxMonitor(const LinuxMonitor&) = delete;
    LinuxMonitor& operator=(const LinuxMonitor&) = delete;

    // ResourceMonitorLike interface
    Result<ResourceSnapshot> read();
    float cpu_usage();
    uint64_t memory_available();
    bool is_throttled();
    void start();
    void stop();

    // Threshold observers
    void on_cpu_threshold(float percent, ThresholdCallback cb);
    void on_memory_threshold(float percent, ThresholdCallback cb);

    // Internal types exposed for implementation (do not use externally)
    struct CpuTimesInternal {
        uint64_t user{0}, nice{0}, system{0}, idle{0};
        uint64_t iowait{0}, irq{0}, softirq{0}, steal{0};
    };

    struct NetCountersInternal {
        uint64_t rx_bytes{0};
        uint64_t tx_bytes{0};
    };

private:

    void sampling_loop(std::stop_token stop);
    ResourceSnapshot sample_once();
    void check_thresholds(const ResourceSnapshot& snap);

    NodeId node_id_;
    uint32_t interval_ms_;
    std::jthread sampling_thread_;
    std::atomic<std::shared_ptr<ResourceSnapshot>> latest_;

    // Previous sample state for delta-based metrics
    CpuTimesInternal prev_cpu_times_{};
    std::array<CpuTimesInternal, 4> prev_per_core_times_{};
    NetCountersInternal prev_net_{};

    // Threshold callbacks
    std::mutex callbacks_mutex_;
    std::vector<std::pair<float, ThresholdCallback>> cpu_callbacks_;
    std::vector<std::pair<float, ThresholdCallback>> memory_callbacks_;
};

// ─────────────────────────────────────────────
// MockMonitor
// ─────────────────────────────────────────────

/**
 * @brief Mock resource monitor for testing and simulation.
 *
 * Returns predetermined resource sequences or a static snapshot.
 * Satisfies ResourceMonitorLike.
 */
class MockMonitor {
public:
    explicit MockMonitor(NodeId node_id, uint32_t sampling_interval_ms = 0);

    // ResourceMonitorLike interface
    Result<ResourceSnapshot> read();
    float cpu_usage();
    uint64_t memory_available();
    bool is_throttled();
    void start();
    void stop();

    // Test helpers — configure what snapshots are returned
    void push_snapshot(ResourceSnapshot snapshot);
    void set_static_snapshot(ResourceSnapshot snapshot);
    void set_cpu(float percent);
    void set_memory(uint64_t available, uint64_t total);
    void set_throttled(bool throttled);

private:
    NodeId node_id_;
    std::vector<ResourceSnapshot> sequence_;
    size_t index_{0};
    ResourceSnapshot static_snapshot_;
    bool use_static_{true};
};

// Verify concept satisfaction at compile time
static_assert(ResourceMonitorLike<LinuxMonitor>);
static_assert(ResourceMonitorLike<MockMonitor>);

}  // namespace edge_orchestrator
