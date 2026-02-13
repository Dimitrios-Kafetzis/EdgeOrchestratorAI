/**
 * @file greedy_policy.hpp
 * @brief Greedy scheduling policy — assigns tasks to least-loaded nodes.
 * @author Dimitris Kafetzis
 */

#pragma once

#include "scheduler/scheduler.hpp"

namespace edge_orchestrator {

class GreedyPolicy : public ISchedulingPolicy {
public:
    SchedulingPlan schedule(const WorkloadDAG& dag,
                            const ResourceSnapshot& local,
                            const ClusterView& cluster) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "greedy"; }
};

}  // namespace edge_orchestrator
