/**
 * @file threshold_policy.cpp
 * @brief ThresholdPolicy — executes locally until resource thresholds are
 *        breached, then offloads to the least-loaded peer.
 * @author Dimitris Kafetzis
 *
 * Algorithm:
 *   For each task in topological order:
 *     If local CPU < threshold AND local memory < threshold:
 *       Assign locally
 *     Else:
 *       Find least-loaded peer
 *       If peer exists and has capacity: offload
 *       Else: queue locally (fallback)
 *
 * Complexity: O(T × log N) with a sorted peer list.
 */

#include "scheduler/threshold_policy.hpp"

#include <algorithm>
#include <limits>

namespace edge_orchestrator {

ThresholdPolicy::ThresholdPolicy(ThresholdPolicyConfig config)
    : config_(config) {}

SchedulingPlan ThresholdPolicy::schedule(const WorkloadDAG& dag,
                                          const ResourceSnapshot& local,
                                          const ClusterView& cluster) {
    SchedulingPlan plan;
    plan.computed_at = std::chrono::system_clock::now();

    auto topo_order = dag.topological_order();

    // Track simulated local resource consumption
    float sim_cpu = local.cpu_usage_percent;
    uint64_t sim_mem_used = local.memory_total_bytes - local.memory_available_bytes;
    uint64_t mem_total = local.memory_total_bytes;
    Duration local_compute{0};

    // Track simulated peer load for balancing offloaded tasks
    struct PeerState {
        NodeId id;
        float cpu_load;
        uint64_t mem_available;
        Duration assigned_compute{0};
    };

    std::vector<PeerState> peers;
    for (const auto& peer : cluster.peers) {
        if (peer.reachable) {
            peers.push_back({
                peer.node_id,
                peer.resources.cpu_usage_percent,
                peer.resources.memory_available_bytes,
                Duration{0}
            });
        }
    }

    for (const auto& task_id : topo_order) {
        auto task_opt = dag.get_task(task_id);
        if (!task_opt) continue;

        const auto& task = *task_opt;

        // Compute simulated local memory usage percentage
        float sim_mem_pct = mem_total > 0
            ? 100.0f * static_cast<float>(sim_mem_used) / static_cast<float>(mem_total)
            : 0.0f;

        bool within_cpu_threshold = sim_cpu < config_.cpu_threshold_percent;
        bool within_mem_threshold = sim_mem_pct < config_.memory_threshold_percent;
        bool local_has_memory = (mem_total - sim_mem_used) >= task.profile.memory_bytes;

        if (within_cpu_threshold && within_mem_threshold && local_has_memory) {
            // Execute locally
            plan.decisions.push_back({
                .task_id = task_id,
                .assigned_node = local.node_id,
                .reason = SchedulingDecision::Reason::LocalCapacity
            });

            local_compute += task.profile.compute_cost;
            sim_mem_used += task.profile.memory_bytes;

            // Simulate CPU increase proportional to task compute cost
            sim_cpu += static_cast<float>(task.profile.compute_cost.count()) * 0.001f;
            sim_cpu = std::min(sim_cpu, 100.0f);

        } else {
            // Threshold breached — try to offload
            // Find the peer with the lowest CPU load that has enough memory
            size_t best_peer = peers.size();  // invalid index = not found
            float best_load = std::numeric_limits<float>::max();

            for (size_t i = 0; i < peers.size(); ++i) {
                if (peers[i].mem_available >= task.profile.memory_bytes &&
                    peers[i].cpu_load < best_load) {
                    best_load = peers[i].cpu_load;
                    best_peer = i;
                }
            }

            if (best_peer < peers.size()) {
                // Offload to peer
                plan.decisions.push_back({
                    .task_id = task_id,
                    .assigned_node = peers[best_peer].id,
                    .reason = SchedulingDecision::Reason::ThresholdOffload
                });

                peers[best_peer].assigned_compute += task.profile.compute_cost;
                peers[best_peer].mem_available -= task.profile.memory_bytes;
                peers[best_peer].cpu_load +=
                    static_cast<float>(task.profile.compute_cost.count()) * 0.001f;
            } else {
                // No suitable peer — fallback to local
                plan.decisions.push_back({
                    .task_id = task_id,
                    .assigned_node = local.node_id,
                    .reason = SchedulingDecision::Reason::Fallback
                });
                local_compute += task.profile.compute_cost;
                sim_mem_used += task.profile.memory_bytes;
            }
        }
    }

    // Estimate makespan
    Duration max_peer_compute{0};
    for (const auto& peer : peers) {
        max_peer_compute = std::max(max_peer_compute, peer.assigned_compute);
    }
    plan.estimated_makespan = std::max(local_compute, max_peer_compute);

    return plan;
}

}  // namespace edge_orchestrator
