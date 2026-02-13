/**
 * @file optimizer_policy.hpp
 * @brief Optimization-based scheduling policy — minimizes makespan.
 * @author Dimitris Kafetzis
 *
 * Inspired by research on dynamic partitioning of DNN/LLM inference
 * across resource-constrained edge devices. Implements a fast heuristic:
 * critical-path assignment + bin-packing + local search.
 */

#pragma once

#include "core/config.hpp"
#include "scheduler/scheduler.hpp"

namespace edge_orchestrator {

class OptimizerPolicy : public ISchedulingPolicy {
public:
    explicit OptimizerPolicy(OptimizerPolicyConfig config);

    SchedulingPlan schedule(const WorkloadDAG& dag,
                            const ResourceSnapshot& local,
                            const ClusterView& cluster) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "optimizer"; }

private:
    OptimizerPolicyConfig config_;

    // Heuristic phases
    SchedulingPlan critical_path_assignment(const WorkloadDAG& dag,
                                            const ResourceSnapshot& local,
                                            const ClusterView& cluster);
    void bin_pack_remaining(SchedulingPlan& plan,
                            const WorkloadDAG& dag,
                            const ResourceSnapshot& local,
                            const ClusterView& cluster);
    void local_search_improve(SchedulingPlan& plan,
                              const WorkloadDAG& dag,
                              const ResourceSnapshot& local,
                              const ClusterView& cluster);
};

}  // namespace edge_orchestrator
