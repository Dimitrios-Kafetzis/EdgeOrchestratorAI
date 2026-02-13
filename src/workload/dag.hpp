/**
 * @file dag.hpp
 * @brief Directed Acyclic Graph representation for workload tasks.
 * @author Dimitris Kafetzis
 *
 * Models computational workloads as DAGs where nodes are tasks and edges
 * represent data dependencies. Provides topological ordering, cycle
 * detection, critical path analysis, and ready-task queries.
 */

#pragma once

#include "core/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace edge_orchestrator {

/**
 * @brief A single task node in the workload DAG.
 */
struct Task {
    TaskId id;
    std::string name;
    TaskProfile profile;
    std::vector<TaskId> dependencies;
    TaskState state = TaskState::Pending;

    auto operator<=>(const Task&) const = default;
};

/**
 * @brief Directed Acyclic Graph of computational tasks.
 */
class WorkloadDAG {
public:
    WorkloadDAG() = default;

    // ── Construction ──────────────────────────
    TaskId add_task(Task task);
    void add_dependency(const TaskId& from, const TaskId& to);

    // ── Queries ───────────────────────────────
    [[nodiscard]] std::vector<TaskId> topological_order() const;
    [[nodiscard]] std::vector<TaskId> ready_tasks() const;
    [[nodiscard]] std::vector<TaskId> dependents(const TaskId& id) const;
    [[nodiscard]] std::vector<TaskId> dependencies(const TaskId& id) const;
    [[nodiscard]] bool is_valid() const;
    [[nodiscard]] bool has_cycle() const;
    [[nodiscard]] size_t task_count() const noexcept;
    [[nodiscard]] std::optional<Task> get_task(const TaskId& id) const;

    // ── State Updates ─────────────────────────
    void mark_completed(const TaskId& id);
    void mark_failed(const TaskId& id);
    void mark_running(const TaskId& id);
    void mark_scheduled(const TaskId& id);

    // ── Metrics ───────────────────────────────
    [[nodiscard]] Duration critical_path_cost() const;
    [[nodiscard]] uint64_t peak_memory_estimate() const;
    [[nodiscard]] Duration total_compute_cost() const;

private:
    std::unordered_map<TaskId, Task> tasks_;
    std::unordered_map<TaskId, std::vector<TaskId>> adj_list_;       // forward edges
    std::unordered_map<TaskId, std::vector<TaskId>> reverse_adj_;    // backward edges
};

}  // namespace edge_orchestrator
