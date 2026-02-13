/**
 * @file optimizer_policy.cpp
 * @brief OptimizerPolicy — minimizes makespan via critical-path assignment,
 *        bin-packing, and local search heuristic.
 * @author Dimitris Kafetzis
 *
 * Inspired by research on dynamic partitioning of DNN/LLM inference across
 * resource-constrained edge devices. The optimization proceeds in three phases:
 *
 * Phase 1: Critical Path Assignment
 *   Identify the DAG's critical path and assign its tasks to minimize
 *   inter-node communication (preferring co-location).
 *
 * Phase 2: Bin Packing (First Fit Decreasing)
 *   Assign remaining tasks sorted by compute cost (largest first) to the
 *   node with the most remaining capacity that satisfies memory constraints.
 *
 * Phase 3: Local Search Improvement
 *   Iteratively try swapping task assignments between nodes. Accept a swap
 *   if it reduces the estimated makespan. Stop after max_iterations or
 *   when no improvement is found.
 *
 * Complexity: O(T × N × I) where T = tasks, N = nodes, I = iterations.
 */

#include "scheduler/optimizer_policy.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace edge_orchestrator {

namespace {

/**
 * @brief Per-node state during optimization.
 */
struct NodeBudget {
    NodeId id;
    uint64_t memory_capacity;
    uint64_t memory_used{0};
    Duration assigned_compute{0};
    std::vector<TaskId> assigned_tasks;

    [[nodiscard]] uint64_t memory_remaining() const {
        return memory_capacity > memory_used ? memory_capacity - memory_used : 0;
    }
};

/**
 * @brief Compute estimated makespan from current assignment.
 * Accounts for inter-node communication costs.
 */
Duration estimate_makespan(const std::vector<NodeBudget>& nodes,
                           const WorkloadDAG& dag,
                           const std::unordered_map<TaskId, size_t>& assignment,
                           float comm_weight) {
    // For each node, compute the total time including communication delays
    // Simple model: makespan = max over nodes of (compute + incoming transfer)
    Duration max_time{0};

    for (const auto& node : nodes) {
        Duration node_time = node.assigned_compute;

        // Add communication cost for tasks with remote dependencies
        for (const auto& task_id : node.assigned_tasks) {
            auto deps = dag.dependencies(task_id);
            for (const auto& dep_id : deps) {
                auto dep_it = assignment.find(dep_id);
                auto task_it = assignment.find(task_id);
                if (dep_it != assignment.end() && task_it != assignment.end()) {
                    if (dep_it->second != task_it->second) {
                        // Cross-node dependency — add transfer cost
                        auto dep_task = dag.get_task(dep_id);
                        if (dep_task) {
                            auto transfer_us = static_cast<int64_t>(
                                static_cast<float>(dep_task->profile.output_bytes) *
                                comm_weight * 0.001f);  // Weighted transfer time
                            node_time += Duration{transfer_us};
                        }
                    }
                }
            }
        }

        max_time = std::max(max_time, node_time);
    }

    return max_time;
}

}  // anonymous namespace

// ─────────────────────────────────────────────
// OptimizerPolicy
// ─────────────────────────────────────────────

OptimizerPolicy::OptimizerPolicy(OptimizerPolicyConfig config)
    : config_(config) {}

SchedulingPlan OptimizerPolicy::schedule(const WorkloadDAG& dag,
                                          const ResourceSnapshot& local,
                                          const ClusterView& cluster) {
    // Phase 1: Critical path assignment
    auto plan = critical_path_assignment(dag, local, cluster);

    // Phase 2: Bin-pack remaining tasks
    bin_pack_remaining(plan, dag, local, cluster);

    // Phase 3: Local search improvement
    local_search_improve(plan, dag, local, cluster);

    return plan;
}

// ─────────────────────────────────────────────
// Phase 1: Critical Path Assignment
// ─────────────────────────────────────────────

SchedulingPlan OptimizerPolicy::critical_path_assignment(
    const WorkloadDAG& dag,
    const ResourceSnapshot& local,
    const ClusterView& /*cluster*/) {

    SchedulingPlan plan;
    plan.computed_at = std::chrono::system_clock::now();

    // Identify the critical path (longest-cost path through the DAG)
    auto topo = dag.topological_order();
    if (topo.empty()) return plan;

    // Compute longest path distances
    std::unordered_map<TaskId, int64_t> dist;
    std::unordered_map<TaskId, TaskId> predecessor;
    for (const auto& id : topo) {
        dist[id] = 0;
    }

    for (const auto& u : topo) {
        auto task = dag.get_task(u);
        if (!task) continue;

        auto rev = dag.dependencies(u);
        if (rev.empty()) {
            dist[u] = task->profile.compute_cost.count();
        }

        for (const auto& v : dag.dependents(u)) {
            auto v_task = dag.get_task(v);
            if (!v_task) continue;
            int64_t candidate = dist[u] + v_task->profile.compute_cost.count();
            if (candidate > dist[v]) {
                dist[v] = candidate;
                predecessor[v] = u;
            }
        }
    }

    // Trace back the critical path from the node with maximum distance
    TaskId end_task;
    int64_t max_dist = -1;
    for (const auto& [id, d] : dist) {
        if (d > max_dist) {
            max_dist = d;
            end_task = id;
        }
    }

    std::unordered_set<TaskId> critical_set;
    {
        TaskId curr = end_task;
        while (!curr.empty()) {
            critical_set.insert(curr);
            auto it = predecessor.find(curr);
            curr = (it != predecessor.end()) ? it->second : "";
        }
    }

    // Assign all critical-path tasks to the local node (minimize communication
    // along the critical path — the most latency-sensitive chain)
    for (const auto& task_id : topo) {
        if (critical_set.count(task_id)) {
            plan.decisions.push_back({
                .task_id = task_id,
                .assigned_node = local.node_id,
                .reason = SchedulingDecision::Reason::OptimizationResult
            });
        }
    }

    return plan;
}

// ─────────────────────────────────────────────
// Phase 2: Bin Pack Remaining (First Fit Decreasing)
// ─────────────────────────────────────────────

void OptimizerPolicy::bin_pack_remaining(
    SchedulingPlan& plan,
    const WorkloadDAG& dag,
    const ResourceSnapshot& local,
    const ClusterView& cluster) {

    // Build set of already-assigned tasks
    std::unordered_set<TaskId> assigned;
    for (const auto& d : plan.decisions) {
        assigned.insert(d.task_id);
    }

    // Build node budgets
    std::vector<NodeBudget> nodes;
    nodes.push_back({local.node_id, local.memory_available_bytes, 0, Duration{0}, {}});
    for (const auto& peer : cluster.peers) {
        if (peer.reachable) {
            nodes.push_back({peer.node_id, peer.resources.memory_available_bytes, 0, Duration{0}, {}});
        }
    }

    // Account for already-assigned tasks' memory usage
    for (const auto& d : plan.decisions) {
        auto task = dag.get_task(d.task_id);
        if (!task) continue;
        for (auto& node : nodes) {
            if (node.id == d.assigned_node) {
                node.memory_used += task->profile.memory_bytes;
                node.assigned_compute += task->profile.compute_cost;
                node.assigned_tasks.push_back(d.task_id);
                break;
            }
        }
    }

    // Collect unassigned tasks, sorted by compute cost descending (FFD)
    auto topo = dag.topological_order();
    std::vector<TaskId> remaining;
    for (const auto& id : topo) {
        if (!assigned.count(id)) {
            remaining.push_back(id);
        }
    }

    std::sort(remaining.begin(), remaining.end(),
              [&dag](const TaskId& a, const TaskId& b) {
                  auto ta = dag.get_task(a);
                  auto tb = dag.get_task(b);
                  return (ta ? ta->profile.compute_cost : Duration{0}) >
                         (tb ? tb->profile.compute_cost : Duration{0});
              });

    // Assign each remaining task to the node with the least assigned compute
    // that has sufficient memory (First Fit Decreasing)
    for (const auto& task_id : remaining) {
        auto task = dag.get_task(task_id);
        if (!task) continue;

        // Find node with minimum assigned compute that fits
        size_t best = nodes.size();
        Duration min_compute = Duration{std::numeric_limits<int64_t>::max()};

        for (size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].memory_remaining() >= task->profile.memory_bytes &&
                nodes[i].assigned_compute < min_compute) {
                min_compute = nodes[i].assigned_compute;
                best = i;
            }
        }

        if (best >= nodes.size()) {
            // No node fits — assign to local as fallback
            best = 0;
        }

        plan.decisions.push_back({
            .task_id = task_id,
            .assigned_node = nodes[best].id,
            .reason = SchedulingDecision::Reason::OptimizationResult
        });

        nodes[best].memory_used += task->profile.memory_bytes;
        nodes[best].assigned_compute += task->profile.compute_cost;
        nodes[best].assigned_tasks.push_back(task_id);
    }
}

// ─────────────────────────────────────────────
// Phase 3: Local Search Improvement
// ─────────────────────────────────────────────

void OptimizerPolicy::local_search_improve(
    SchedulingPlan& plan,
    const WorkloadDAG& dag,
    const ResourceSnapshot& local,
    const ClusterView& cluster) {

    if (plan.decisions.size() < 2) return;

    // Build node budgets from current assignment
    std::vector<NodeBudget> nodes;
    nodes.push_back({local.node_id, local.memory_available_bytes, 0, Duration{0}, {}});
    for (const auto& peer : cluster.peers) {
        if (peer.reachable) {
            nodes.push_back({peer.node_id, peer.resources.memory_available_bytes, 0, Duration{0}, {}});
        }
    }

    if (nodes.size() < 2) return;  // No point swapping with only one node

    // Build task → node_index assignment map
    std::unordered_map<TaskId, size_t> assignment;
    std::unordered_map<NodeId, size_t> node_index;
    for (size_t i = 0; i < nodes.size(); ++i) {
        node_index[nodes[i].id] = i;
    }

    for (auto& d : plan.decisions) {
        auto task = dag.get_task(d.task_id);
        if (!task) continue;

        auto ni = node_index.find(d.assigned_node);
        if (ni == node_index.end()) continue;

        size_t idx = ni->second;
        assignment[d.task_id] = idx;
        nodes[idx].memory_used += task->profile.memory_bytes;
        nodes[idx].assigned_compute += task->profile.compute_cost;
        nodes[idx].assigned_tasks.push_back(d.task_id);
    }

    Duration current_makespan = estimate_makespan(nodes, dag, assignment, config_.communication_weight);

    // Iterative improvement: try random task swaps
    std::mt19937 rng(42);  // Deterministic for reproducibility

    for (uint32_t iter = 0; iter < config_.max_iterations; ++iter) {
        if (plan.decisions.empty()) break;

        // Pick a random task
        std::uniform_int_distribution<size_t> task_dist(0, plan.decisions.size() - 1);
        size_t task_idx = task_dist(rng);
        const auto& task_id = plan.decisions[task_idx].task_id;

        auto task = dag.get_task(task_id);
        if (!task) continue;

        size_t from_node = assignment[task_id];

        // Pick a random different node to try
        std::uniform_int_distribution<size_t> node_dist(0, nodes.size() - 1);
        size_t to_node = node_dist(rng);
        if (to_node == from_node) continue;

        // Check memory feasibility
        if (nodes[to_node].memory_remaining() < task->profile.memory_bytes) continue;

        // Tentatively swap
        nodes[from_node].assigned_compute -= task->profile.compute_cost;
        nodes[from_node].memory_used -= task->profile.memory_bytes;
        auto& from_tasks = nodes[from_node].assigned_tasks;
        from_tasks.erase(std::find(from_tasks.begin(), from_tasks.end(), task_id));

        nodes[to_node].assigned_compute += task->profile.compute_cost;
        nodes[to_node].memory_used += task->profile.memory_bytes;
        nodes[to_node].assigned_tasks.push_back(task_id);

        assignment[task_id] = to_node;

        Duration new_makespan = estimate_makespan(nodes, dag, assignment, config_.communication_weight);

        if (new_makespan < current_makespan) {
            // Accept the swap
            current_makespan = new_makespan;
            plan.decisions[task_idx].assigned_node = nodes[to_node].id;
        } else {
            // Revert
            nodes[to_node].assigned_compute -= task->profile.compute_cost;
            nodes[to_node].memory_used -= task->profile.memory_bytes;
            nodes[to_node].assigned_tasks.pop_back();

            nodes[from_node].assigned_compute += task->profile.compute_cost;
            nodes[from_node].memory_used += task->profile.memory_bytes;
            nodes[from_node].assigned_tasks.push_back(task_id);

            assignment[task_id] = from_node;
        }
    }

    plan.estimated_makespan = current_makespan;
}

}  // namespace edge_orchestrator
