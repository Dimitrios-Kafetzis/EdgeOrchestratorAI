/**
 * @file greedy_policy.cpp
 * @brief GreedyPolicy — assigns each ready task to the node with the
 *        most available resource headroom.
 * @author Dimitris Kafetzis
 *
 * Algorithm:
 *   For each task in topological order:
 *     If task is ready:
 *       score(node) = w_cpu * cpu_headroom(node)
 *                   + w_mem * memory_headroom(node)
 *                   - w_net * transfer_cost(task, node)
 *       Assign task to argmax(score)
 *
 * Complexity: O(T × N) where T = tasks, N = nodes.
 */

#include "scheduler/greedy_policy.hpp"

#include <algorithm>
#include <limits>

namespace edge_orchestrator {

namespace {

constexpr float W_CPU = 1.0f;
constexpr float W_MEM = 0.5f;
constexpr float W_NET = 0.3f;

struct NodeScore {
    NodeId node_id;
    float score;
};

float compute_node_score(const ResourceSnapshot& resources,
                         const TaskProfile& task_profile,
                         bool is_local) {
    float cpu_headroom = resources.cpu_headroom_percent();
    float mem_headroom = 100.0f - resources.memory_usage_percent();

    // Transfer cost is zero for local execution
    float transfer_cost = is_local ? 0.0f
        : static_cast<float>(task_profile.input_bytes + task_profile.output_bytes)
          / (1024.0f * 1024.0f);  // Normalize to MB

    return W_CPU * cpu_headroom + W_MEM * mem_headroom - W_NET * transfer_cost;
}

}  // anonymous namespace

SchedulingPlan GreedyPolicy::schedule(const WorkloadDAG& dag,
                                       const ResourceSnapshot& local,
                                       const ClusterView& cluster) {
    SchedulingPlan plan;
    plan.computed_at = std::chrono::system_clock::now();

    auto topo_order = dag.topological_order();

    // Track simulated resource consumption per node
    struct NodeState {
        NodeId id;
        ResourceSnapshot resources;
        Duration assigned_compute{0};
    };

    // Build node list: local + all reachable peers
    std::vector<NodeState> nodes;
    nodes.push_back({local.node_id, local, Duration{0}});

    for (const auto& peer : cluster.peers) {
        if (peer.reachable) {
            nodes.push_back({peer.node_id, peer.resources, Duration{0}});
        }
    }

    // Assign each task greedily
    for (const auto& task_id : topo_order) {
        auto task_opt = dag.get_task(task_id);
        if (!task_opt) continue;

        const auto& task = *task_opt;

        // Find best node
        float best_score = -std::numeric_limits<float>::max();
        size_t best_idx = 0;

        for (size_t i = 0; i < nodes.size(); ++i) {
            bool is_local = (i == 0);

            // Check memory constraint
            if (task.profile.memory_bytes > nodes[i].resources.memory_available_bytes) {
                continue;
            }

            float score = compute_node_score(nodes[i].resources, task.profile, is_local);

            if (score > best_score) {
                best_score = score;
                best_idx = i;
            }
        }

        // Record decision
        SchedulingDecision decision{
            .task_id = task_id,
            .assigned_node = nodes[best_idx].id,
            .reason = (best_idx == 0)
                ? SchedulingDecision::Reason::LocalCapacity
                : SchedulingDecision::Reason::LeastLoaded
        };
        plan.decisions.push_back(decision);

        // Update simulated node state
        nodes[best_idx].assigned_compute += task.profile.compute_cost;
        if (nodes[best_idx].resources.memory_available_bytes >= task.profile.memory_bytes) {
            nodes[best_idx].resources.memory_available_bytes -= task.profile.memory_bytes;
        }
    }

    // Estimate makespan as the maximum assigned compute across all nodes
    Duration max_compute{0};
    for (const auto& node : nodes) {
        max_compute = std::max(max_compute, node.assigned_compute);
    }
    plan.estimated_makespan = max_compute;

    return plan;
}

}  // namespace edge_orchestrator
