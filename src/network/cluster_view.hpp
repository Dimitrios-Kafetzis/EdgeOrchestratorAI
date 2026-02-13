/**
 * @file cluster_view.hpp
 * @brief Thread-safe aggregated view of all cluster peers.
 * @author Dimitris Kafetzis
 */

#pragma once

#include "core/types.hpp"
#include "scheduler/scheduler.hpp"

#include <chrono>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace edge_orchestrator {

/**
 * @brief Maintains a consistent view of cluster peer states.
 *
 * Updated by PeerDiscovery, read by Scheduler. Thread-safe via shared_mutex.
 */
class ClusterViewManager {
public:
    void update_peer(const NodeId& id, ResourceSnapshot snapshot);
    void mark_unreachable(const NodeId& id);
    void remove_peer(const NodeId& id);
    void clear();

    [[nodiscard]] ClusterView snapshot() const;
    [[nodiscard]] std::vector<NodeId> available_peers() const;
    [[nodiscard]] std::optional<ResourceSnapshot> peer_resources(const NodeId& id) const;
    [[nodiscard]] size_t cluster_size() const noexcept;
    [[nodiscard]] bool has_peer(const NodeId& id) const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<NodeId, PeerInfo> peers_;
};

}  // namespace edge_orchestrator
