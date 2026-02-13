/**
 * @file scheduler.hpp
 * @brief Scheduler types, plan structures, and policy variant.
 * @author Dimitris Kafetzis
 */

#pragma once

#include "core/types.hpp"
#include "workload/dag.hpp"

#include <string_view>
#include <variant>
#include <vector>

namespace edge_orchestrator {

// ─────────────────────────────────────────────
// Cluster View (aggregated peer state)
// ─────────────────────────────────────────────

struct PeerInfo {
    NodeId node_id;
    ResourceSnapshot resources;
    bool reachable = true;
};

struct ClusterView {
    std::vector<PeerInfo> peers;

    [[nodiscard]] size_t available_count() const noexcept;
    [[nodiscard]] NodeId least_loaded_peer() const;
};

// ─────────────────────────────────────────────
// Scheduling Decision & Plan
// ─────────────────────────────────────────────

struct SchedulingDecision {
    TaskId task_id;
    NodeId assigned_node;

    enum class Reason : uint8_t {
        LocalCapacity,
        LeastLoaded,
        ThresholdOffload,
        OptimizationResult,
        Fallback
    } reason;
};

struct SchedulingPlan {
    std::vector<SchedulingDecision> decisions;
    Duration estimated_makespan{0};
    Timestamp computed_at;
};

/**
 * @brief Abstract interface for scheduling policies (runtime polymorphism).
 *
 * Complements the SchedulingPolicyLike concept with a virtual interface
 * for use in contexts requiring type-erased policy selection (Orchestrator).
 */
class ISchedulingPolicy {
public:
    virtual ~ISchedulingPolicy() = default;
    virtual SchedulingPlan schedule(const WorkloadDAG& dag,
                                    const ResourceSnapshot& local,
                                    const ClusterView& cluster) = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

}  // namespace edge_orchestrator
