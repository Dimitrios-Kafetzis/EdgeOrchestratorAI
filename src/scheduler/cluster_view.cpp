/**
 * @file cluster_view.cpp
 * @brief ClusterView helper method implementations.
 * @author Dimitris Kafetzis
 */

#include "scheduler/scheduler.hpp"

#include <algorithm>
#include <limits>

namespace edge_orchestrator {

size_t ClusterView::available_count() const noexcept {
    return static_cast<size_t>(
        std::count_if(peers.begin(), peers.end(),
                      [](const PeerInfo& p) { return p.reachable; }));
}

NodeId ClusterView::least_loaded_peer() const {
    float min_load = std::numeric_limits<float>::max();
    NodeId best;

    for (const auto& peer : peers) {
        if (!peer.reachable) continue;
        if (peer.resources.cpu_usage_percent < min_load) {
            min_load = peer.resources.cpu_usage_percent;
            best = peer.node_id;
        }
    }

    return best;
}

}  // namespace edge_orchestrator
