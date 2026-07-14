/**
 * @file cluster_view.cpp
 * @brief ClusterViewManager implementation.
 * @author Dimitris Kafetzis
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
