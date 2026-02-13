/**
 * @file task_runner.hpp
 * @brief Task execution with budget enforcement.
 * @author Dimitris Kafetzis
 */

#pragma once

#include "core/types.hpp"
#include "executor/memory_pool.hpp"

#include <optional>
#include <string>
#include <thread>

namespace edge_orchestrator {

struct ExecutionResult {
    TaskId task_id;
    TaskState final_state;
    Duration actual_duration;
    uint64_t peak_memory_bytes;
    std::optional<std::string> error_message;
};

/**
 * @brief Executes synthetic tasks with memory and time budget enforcement.
 */
class TaskRunner {
public:
    ExecutionResult execute(const TaskId& task_id,
                            const TaskProfile& profile,
                            MemoryPool& pool,
                            std::stop_token stop);

private:
    void simulate_compute(Duration target_duration, std::stop_token stop);
};

}  // namespace edge_orchestrator
