/**
 * @file types.hpp
 * @brief Fundamental types used throughout EdgeOrchestrator.
 * @author Dimitris Kafetzis
 *
 * Defines NodeId, TaskId, ResourceSnapshot, and other shared vocabulary types.
 * All types are designed for value semantics and zero-cost abstractions.
 */

#pragma once

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <string>

namespace edge_orchestrator {

// ─────────────────────────────────────────────
// Identity Types
// ─────────────────────────────────────────────

using NodeId = std::string;
using TaskId = std::string;
using Timestamp = std::chrono::system_clock::time_point;
using Duration = std::chrono::microseconds;
using SteadyTime = std::chrono::steady_clock::time_point;

// ─────────────────────────────────────────────
// Resource Snapshot
// ─────────────────────────────────────────────

/**
 * @brief A point-in-time snapshot of a node's hardware resources.
 *
 * Read from Linux pseudo-filesystems (/proc, /sys) by the ResourceMonitor.
 * Immutable once constructed — safe for lock-free sharing via atomic shared_ptr.
 */
struct ResourceSnapshot {
    NodeId node_id;
    Timestamp timestamp;

    float cpu_usage_percent{0.0f};                    ///< Aggregate CPU [0.0, 100.0]
    std::array<float, 4> per_core_cpu_percent{};      ///< Per-core (RPi4 has 4 cores)

    uint64_t memory_available_bytes{0};
    uint64_t memory_total_bytes{0};

    float cpu_temperature_celsius{0.0f};
    float gpu_temperature_celsius{0.0f};

    uint64_t network_rx_bytes_sec{0};                 ///< Receive throughput
    uint64_t network_tx_bytes_sec{0};                 ///< Transmit throughput

    bool is_throttled{false};                         ///< Thermal throttling active

    [[nodiscard]] constexpr float memory_usage_percent() const noexcept {
        if (memory_total_bytes == 0) return 0.0f;
        return 100.0f * static_cast<float>(memory_total_bytes - memory_available_bytes)
               / static_cast<float>(memory_total_bytes);
    }

    [[nodiscard]] constexpr float cpu_headroom_percent() const noexcept {
        return 100.0f - cpu_usage_percent;
    }
};

// ─────────────────────────────────────────────
// Task Profile
// ─────────────────────────────────────────────

/**
 * @brief Cost profile for a computational task.
 *
 * Models the resource requirements of a single unit of work (e.g., one
 * transformer layer). Used by the scheduler to make placement decisions.
 */
struct TaskProfile {
    Duration compute_cost{0};                 ///< Expected execution time
    uint64_t memory_bytes{0};                 ///< Working memory requirement
    uint64_t input_bytes{0};                  ///< Input data size (transfer cost)
    uint64_t output_bytes{0};                 ///< Output data size (transfer cost)

    auto operator<=>(const TaskProfile&) const = default;
};

// ─────────────────────────────────────────────
// Task State
// ─────────────────────────────────────────────

enum class TaskState : uint8_t {
    Pending,       ///< Waiting for dependencies
    Ready,         ///< All dependencies met, awaiting scheduling
    Scheduled,     ///< Assigned to a node
    Running,       ///< Currently executing
    Completed,     ///< Finished successfully
    Failed         ///< Execution failed
};

/**
 * @brief Convert TaskState to string representation.
 */
[[nodiscard]] constexpr std::string_view to_string(TaskState state) noexcept {
    switch (state) {
        case TaskState::Pending:    return "pending";
        case TaskState::Ready:      return "ready";
        case TaskState::Scheduled:  return "scheduled";
        case TaskState::Running:    return "running";
        case TaskState::Completed:  return "completed";
        case TaskState::Failed:     return "failed";
    }
    return "unknown";
}

}  // namespace edge_orchestrator
