/**
 * @file cluster_view.cpp
 * @brief ClusterViewManager implementation.
 * @author Dimitris Kafetzis
 *
 * The manager is the single mutable home of peer state; everything a
 * scheduler ever sees is an immutable ClusterView copied out under a
 * shared lock. Copying is the concurrency strategy: discovery threads
 * write rarely (every heartbeat), schedulers read a snapshot and then
 * work on it lock-free for the whole scheduling pass. A shared_mutex
 * fits that read-mostly profile; peers number in the tens, so the copy
 * costs less than a microsecond (measured in bench_scheduler,
 * cluster_snapshot(10) ≈ 0.7 µs on a Pi 4).
 */

#include "network/cluster_view.hpp"

#include <mutex>

namespace edge_orchestrator {

void ClusterViewManager::update_peer(const NodeId& id, ResourceSnapshot snapshot) {
    std::unique_lock lock(mutex_);
    auto& entry = peers_[id];  // preserves a previously learned endpoint
    entry.node_id = id;
    entry.resources = std::move(snapshot);
    entry.reachable = true;
}

void ClusterViewManager::update_peer(const PeerInfo& info) {
    std::unique_lock lock(mutex_);
    peers_[info.node_id] = info;
}

void ClusterViewManager::mark_unreachable(const NodeId& id) {
    std::unique_lock lock(mutex_);
    auto it = peers_.find(id);
    if (it != peers_.end()) {
        it->second.reachable = false;
    }
}

void ClusterViewManager::remove_peer(const NodeId& id) {
    std::unique_lock lock(mutex_);
    peers_.erase(id);
}

void ClusterViewManager::clear() {
    std::unique_lock lock(mutex_);
    peers_.clear();
}

/**
 * @brief Copy out the reachable peers as an immutable view.
 *
 * Unreachable peers are filtered here, not at the call sites, so a
 * policy can never accidentally schedule onto a node that discovery has
 * marked dead but not yet evicted.
 */
ClusterView ClusterViewManager::snapshot() const {
    std::shared_lock lock(mutex_);
    ClusterView view;
    view.peers.reserve(peers_.size());
    for (const auto& [id, info] : peers_) {
        if (info.reachable) {
            view.peers.push_back(info);
        }
    }
    return view;
}

std::vector<NodeId> ClusterViewManager::available_peers() const {
    std::shared_lock lock(mutex_);
    std::vector<NodeId> result;
    for (const auto& [id, info] : peers_) {
        if (info.reachable) result.push_back(id);
    }
    return result;
}

std::optional<ResourceSnapshot> ClusterViewManager::peer_resources(const NodeId& id) const {
    std::shared_lock lock(mutex_);
    auto it = peers_.find(id);
    if (it == peers_.end() || !it->second.reachable) return std::nullopt;
    return it->second.resources;
}

std::optional<PeerInfo> ClusterViewManager::peer_info(const NodeId& id) const {
    std::shared_lock lock(mutex_);
    auto it = peers_.find(id);
    if (it == peers_.end() || !it->second.reachable) return std::nullopt;
    return it->second;
}

size_t ClusterViewManager::cluster_size() const noexcept {
    std::shared_lock lock(mutex_);
    return peers_.size();
}

bool ClusterViewManager::has_peer(const NodeId& id) const {
    std::shared_lock lock(mutex_);
    return peers_.count(id) > 0;
}

}  // namespace edge_orchestrator
