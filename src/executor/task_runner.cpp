/**
 * @file task_runner.cpp
 * @brief TaskRunner implementation with synthetic compute simulation.
 * @author Dimitris Kafetzis
 *
 * Tasks here are resource envelopes, not code: a task "runs" by
 * claiming its declared memory from the arena and burning its declared
 * compute_cost of real CPU time. That is exactly what the scheduling
 * research needs — the policies care about how long tasks occupy a
 * node's resources, not what they compute — and it keeps every
 * benchmark reproducible because a 10 ms task costs 10 ms on any
 * machine. Plugging in real inference later means swapping
 * simulate_compute for a kernel invocation behind the same profile.
 */

#include "executor/task_runner.hpp"

#include <chrono>
#include <thread>

namespace edge_orchestrator {

ExecutionResult TaskRunner::execute(const TaskId& task_id,
                                     const TaskProfile& profile,
                                     MemoryPool& pool,
                                     std::stop_token stop) {
    auto start = std::chrono::steady_clock::now();

    // Allocate working memory from pool
    void* mem = pool.allocate(profile.memory_bytes);
    if (!mem) {
        return ExecutionResult{
            .task_id = task_id,
            .final_state = TaskState::Failed,
            .actual_duration = Duration{0},
            .peak_memory_bytes = 0,
            .error_message = "Out of memory in pool"
        };
    }

    // Simulate computation
    simulate_compute(profile.compute_cost, stop);

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<Duration>(end - start);

    return ExecutionResult{
        .task_id = task_id,
        .final_state = stop.stop_requested() ? TaskState::Failed : TaskState::Completed,
        .actual_duration = duration,
        .peak_memory_bytes = profile.memory_bytes,
        .error_message = stop.stop_requested()
            ? std::optional<std::string>{"Cancelled via stop token"}
            : std::nullopt
    };
}

/**
 * @brief Burn CPU for the target duration, checking for cancellation.
 *
 * A busy loop rather than sleep_for on purpose: sleeping tasks would
 * not contend for cores, and core contention is precisely the effect
 * the resource monitor and the policies are supposed to observe. The
 * inner 1000-iteration burst keeps the steady_clock and stop_token
 * checks off the hot path while bounding cancellation latency to
 * microseconds.
 */
void TaskRunner::simulate_compute(Duration target_duration, std::stop_token stop) {
    auto start = std::chrono::steady_clock::now();
    volatile uint64_t counter = 0;

    while (!stop.stop_requested()) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<Duration>(elapsed) >= target_duration) break;

        // Synthetic work to burn CPU cycles
        for (int i = 0; i < 1000; ++i) {
            counter += static_cast<uint64_t>(i) * static_cast<uint64_t>(i);
        }
    }
}

}  // namespace edge_orchestrator
