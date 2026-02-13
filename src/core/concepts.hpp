/**
 * @file concepts.hpp
 * @brief C++20 concept definitions for EdgeOrchestrator interfaces.
 * @author Dimitris Kafetzis
 *
 * Defines compile-time interface constraints for hot-path components.
 * These concepts enable static polymorphism with zero virtual dispatch overhead.
 */

#pragma once

#include "core/types.hpp"
#include "core/result.hpp"

#include <concepts>
#include <string_view>
#include <vector>

namespace edge_orchestrator {

// Forward declarations
class WorkloadDAG;
struct ClusterView;
struct SchedulingPlan;

// ─────────────────────────────────────────────
// ResourceMonitorLike
// ─────────────────────────────────────────────

/**
 * @concept ResourceMonitorLike
 * @brief Constrains types that can provide resource snapshots.
 *
 * Used on the hot path (called every sampling interval), so we use
 * concept-based static polymorphism instead of virtual dispatch.
 */
template <typename T>
concept ResourceMonitorLike = requires(T monitor) {
    { monitor.read() } -> std::same_as<Result<ResourceSnapshot>>;
    { monitor.cpu_usage() } -> std::convertible_to<float>;
    { monitor.memory_available() } -> std::convertible_to<uint64_t>;
    { monitor.is_throttled() } -> std::convertible_to<bool>;
    { monitor.start() } -> std::same_as<void>;
    { monitor.stop() } -> std::same_as<void>;
};

// ─────────────────────────────────────────────
// SchedulingPolicyLike
// ─────────────────────────────────────────────

/**
 * @concept SchedulingPolicyLike
 * @brief Constrains types that can produce scheduling plans.
 *
 * The scheduler calls the active policy on every scheduling round.
 * Concept-based dispatch eliminates vtable overhead in the inner loop.
 */
template <typename T>
concept SchedulingPolicyLike = requires(
    T policy,
    const WorkloadDAG& dag,
    const ResourceSnapshot& local,
    const ClusterView& cluster
) {
    { policy.schedule(dag, local, cluster) } -> std::same_as<SchedulingPlan>;
    { T::name() } -> std::convertible_to<std::string_view>;
};

// ─────────────────────────────────────────────
// SerializerLike
// ─────────────────────────────────────────────

/**
 * @concept SerializerLike
 * @brief Constrains types that can serialize/deserialize messages.
 *
 * Used in the network layer for every send/receive operation.
 */
template <typename T, typename MessageT>
concept SerializerLike = requires(T serializer, const MessageT& msg, std::span<const std::byte> data) {
    { serializer.serialize(msg) } -> std::same_as<std::vector<std::byte>>;
    { serializer.deserialize(data) } -> std::same_as<Result<MessageT>>;
};

}  // namespace edge_orchestrator
