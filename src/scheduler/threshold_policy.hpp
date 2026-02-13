/**
 * @file threshold_policy.hpp
 * @brief Threshold-based scheduling policy — offloads when load exceeds limits.
 * @author Dimitris Kafetzis
 */

#pragma once

#include "core/config.hpp"
#include "scheduler/scheduler.hpp"

namespace edge_orchestrator {

class ThresholdPolicy : public ISchedulingPolicy {
public:
    explicit ThresholdPolicy(ThresholdPolicyConfig config);

    SchedulingPlan schedule(const WorkloadDAG& dag,
                            const ResourceSnapshot& local,
                            const ClusterView& cluster) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "threshold"; }

private:
    ThresholdPolicyConfig config_;
};

}  // namespace edge_orchestrator
