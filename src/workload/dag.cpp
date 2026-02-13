/**
 * @file dag.cpp
 * @brief WorkloadDAG implementation — full graph algorithms.
 * @author Dimitris Kafetzis
 *
 * Implements Kahn's algorithm for topological ordering, DFS-based cycle
 * detection, longest-path critical path computation, and ready-task queries.
 * All algorithms operate on the internal adjacency lists with O(V+E) complexity.
 */

#include "workload/dag.hpp"

#include <algorithm>
#include <numeric>
#include <queue>
#include <ranges>
#include <stack>
#include <unordered_set>

namespace edge_orchestrator {

// ─────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────

TaskId WorkloadDAG::add_task(Task task) {
    TaskId id = task.id;
    adj_list_[id];
    reverse_adj_[id];
    tasks_.emplace(id, std::move(task));
    return id;
}

void WorkloadDAG::add_dependency(const TaskId& from, const TaskId& to) {
    adj_list_[from].push_back(to);
    reverse_adj_[to].push_back(from);

    if (auto it = tasks_.find(to); it != tasks_.end()) {
        auto& deps = it->second.dependencies;
        if (std::find(deps.begin(), deps.end(), from) == deps.end()) {
            deps.push_back(from);
        }
    }
}

// ─────────────────────────────────────────────
// Topological Ordering (Kahn's Algorithm)
// ─────────────────────────────────────────────

std::vector<TaskId> WorkloadDAG::topological_order() const {
    std::unordered_map<TaskId, size_t> in_degree;
    for (const auto& [id, _] : tasks_) {
        in_degree[id] = 0;
    }
    for (const auto& [from, neighbors] : adj_list_) {
        for (const auto& to : neighbors) {
            ++in_degree[to];
        }
    }

    std::queue<TaskId> zero_in;
    for (const auto& [id, deg] : in_degree) {
        if (deg == 0) {
            zero_in.push(id);
        }
    }

    std::vector<TaskId> order;
    order.reserve(tasks_.size());

    while (!zero_in.empty()) {
        auto current = zero_in.front();
        zero_in.pop();
        order.push_back(current);

        if (auto it = adj_list_.find(current); it != adj_list_.end()) {
            for (const auto& neighbor : it->second) {
                if (--in_degree[neighbor] == 0) {
                    zero_in.push(neighbor);
                }
            }
        }
    }

    return order;
}

// ─────────────────────────────────────────────
// Ready Tasks
// ─────────────────────────────────────────────

std::vector<TaskId> WorkloadDAG::ready_tasks() const {
    std::vector<TaskId> ready;

    for (const auto& [id, task] : tasks_) {
        if (task.state != TaskState::Pending) continue;

        bool all_deps_met = true;
        if (auto it = reverse_adj_.find(id); it != reverse_adj_.end()) {
            for (const auto& dep_id : it->second) {
                if (auto dep_it = tasks_.find(dep_id); dep_it != tasks_.end()) {
                    if (dep_it->second.state != TaskState::Completed) {
                        all_deps_met = false;
                        break;
                    }
                }
            }
        }

        if (all_deps_met) {
            ready.push_back(id);
        }
    }

    return ready;
}

// ─────────────────────────────────────────────
// Query Methods
// ─────────────────────────────────────────────

std::vector<TaskId> WorkloadDAG::dependents(const TaskId& id) const {
    auto it = adj_list_.find(id);
    if (it == adj_list_.end()) return {};
    return it->second;
}

std::vector<TaskId> WorkloadDAG::dependencies(const TaskId& id) const {
    auto it = reverse_adj_.find(id);
    if (it == reverse_adj_.end()) return {};
    return it->second;
}

bool WorkloadDAG::is_valid() const {
    return !tasks_.empty() && !has_cycle();
}

bool WorkloadDAG::has_cycle() const {
    enum class Color : uint8_t { White, Gray, Black };
    std::unordered_map<TaskId, Color> color;

    for (const auto& [id, _] : tasks_) {
        color[id] = Color::White;
    }

    for (const auto& [start_id, _] : tasks_) {
        if (color[start_id] != Color::White) continue;

        struct Frame {
            TaskId node;
            size_t neighbor_idx;
        };

        std::stack<Frame> dfs_stack;
        dfs_stack.push({start_id, 0});
        color[start_id] = Color::Gray;

        while (!dfs_stack.empty()) {
            auto& [node, idx] = dfs_stack.top();

            auto adj_it = adj_list_.find(node);
            if (adj_it == adj_list_.end() || idx >= adj_it->second.size()) {
                color[node] = Color::Black;
                dfs_stack.pop();
                continue;
            }

            const auto& neighbor = adj_it->second[idx];
            ++idx;

            if (color[neighbor] == Color::Gray) {
                return true;
            }
            if (color[neighbor] == Color::White) {
                color[neighbor] = Color::Gray;
                dfs_stack.push({neighbor, 0});
            }
        }
    }

    return false;
}

size_t WorkloadDAG::task_count() const noexcept {
    return tasks_.size();
}

std::optional<Task> WorkloadDAG::get_task(const TaskId& id) const {
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return std::nullopt;
    return it->second;
}

// ─────────────────────────────────────────────
// State Updates
// ─────────────────────────────────────────────

void WorkloadDAG::mark_completed(const TaskId& id) {
    if (auto it = tasks_.find(id); it != tasks_.end())
        it->second.state = TaskState::Completed;
}

void WorkloadDAG::mark_failed(const TaskId& id) {
    if (auto it = tasks_.find(id); it != tasks_.end())
        it->second.state = TaskState::Failed;
}

void WorkloadDAG::mark_running(const TaskId& id) {
    if (auto it = tasks_.find(id); it != tasks_.end())
        it->second.state = TaskState::Running;
}

void WorkloadDAG::mark_scheduled(const TaskId& id) {
    if (auto it = tasks_.find(id); it != tasks_.end())
        it->second.state = TaskState::Scheduled;
}

// ─────────────────────────────────────────────
// Metrics
// ─────────────────────────────────────────────

Duration WorkloadDAG::critical_path_cost() const {
    if (tasks_.empty()) return Duration{0};

    auto topo = topological_order();
    if (topo.size() != tasks_.size()) return Duration{0};

    // Longest-path via DP on topological order
    // dist[v] = length of longest path ending at v (inclusive of v's cost)
    std::unordered_map<TaskId, int64_t> dist;
    for (const auto& [id, _] : tasks_) {
        dist[id] = 0;
    }

    for (const auto& u : topo) {
        auto task_it = tasks_.find(u);
        if (task_it == tasks_.end()) continue;

        int64_t u_cost = task_it->second.profile.compute_cost.count();

        // If u has no predecessors, its dist is its own cost
        if (auto rev_it = reverse_adj_.find(u);
            rev_it == reverse_adj_.end() || rev_it->second.empty()) {
            dist[u] = std::max(dist[u], u_cost);
        }

        // Relax all outgoing edges
        if (auto adj_it = adj_list_.find(u); adj_it != adj_list_.end()) {
            for (const auto& v : adj_it->second) {
                auto v_task_it = tasks_.find(v);
                if (v_task_it != tasks_.end()) {
                    int64_t v_cost = v_task_it->second.profile.compute_cost.count();
                    int64_t candidate = dist[u] + v_cost;
                    if (candidate > dist[v]) {
                        dist[v] = candidate;
                    }
                }
            }
        }
    }

    int64_t max_dist = 0;
    for (const auto& [id, d] : dist) {
        max_dist = std::max(max_dist, d);
    }

    return Duration{max_dist};
}

uint64_t WorkloadDAG::peak_memory_estimate() const {
    if (tasks_.empty()) return 0;

    auto topo = topological_order();
    if (topo.size() != tasks_.size()) return 0;

    // Assign each task a "level" = longest path from any source
    std::unordered_map<TaskId, size_t> level;
    for (const auto& id : topo) {
        size_t max_parent_level = 0;
        if (auto rev_it = reverse_adj_.find(id); rev_it != reverse_adj_.end()) {
            for (const auto& parent : rev_it->second) {
                auto p_it = level.find(parent);
                if (p_it != level.end()) {
                    max_parent_level = std::max(max_parent_level, p_it->second + 1);
                }
            }
        }
        level[id] = max_parent_level;
    }

    // Sum memory per level, return the maximum
    std::unordered_map<size_t, uint64_t> level_memory;
    for (const auto& [id, lvl] : level) {
        auto task_it = tasks_.find(id);
        if (task_it != tasks_.end()) {
            level_memory[lvl] += task_it->second.profile.memory_bytes;
        }
    }

    uint64_t peak = 0;
    for (const auto& [lvl, mem] : level_memory) {
        peak = std::max(peak, mem);
    }

    return peak;
}

Duration WorkloadDAG::total_compute_cost() const {
    int64_t total = 0;
    for (const auto& [id, task] : tasks_) {
        total += task.profile.compute_cost.count();
    }
    return Duration{total};
}

}  // namespace edge_orchestrator
