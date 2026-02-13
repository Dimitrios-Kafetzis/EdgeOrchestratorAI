/**
 * @file peer_discovery.hpp
 * @brief UDP broadcast-based peer discovery for cluster formation.
 * @author Dimitris Kafetzis
 *
 * Sends periodic heartbeat advertisements via UDP broadcast and listens
 * for peer announcements. Peers not heard from within the timeout are
 * marked unreachable and evicted. Integrates with ClusterViewManager
 * to maintain a consistent cluster view.
 */

#pragma once

#include "core/types.hpp"
#include "scheduler/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace edge_orchestrator {

using PeerCallback = std::function<void(const PeerInfo&)>;

class PeerDiscovery {
public:
    PeerDiscovery(NodeId node_id,
                  uint16_t port,
                  uint32_t heartbeat_interval_ms,
                  uint32_t peer_timeout_ms);
    ~PeerDiscovery();

    // Non-copyable
    PeerDiscovery(const PeerDiscovery&) = delete;
    PeerDiscovery& operator=(const PeerDiscovery&) = delete;

    void start();
    void stop();

    void on_peer_discovered(PeerCallback callback);
    void on_peer_lost(PeerCallback callback);

    void update_local_resources(const ResourceSnapshot& snapshot);

    // Query
    [[nodiscard]] size_t known_peer_count() const;

    /**
     * @brief Wire format for UDP advertisement packets.
     *
     * Fixed-size binary format avoids Protobuf dependency for the
     * high-frequency discovery path. 64 bytes total.
     */
    struct __attribute__((packed)) AdvertPacket {
        char magic[4];             // "EORC"
        uint8_t version;           // Protocol version (1)
        uint8_t reserved[3];
        char node_id[32];          // Null-padded node identifier
        uint16_t tcp_port;         // Port for TCP transport
        uint16_t padding;
        float cpu_usage_percent;
        uint64_t memory_available_bytes;
        uint64_t memory_total_bytes;
        float temperature_celsius;
        uint32_t flags;            // Bit 0: is_throttled
    };

    static_assert(sizeof(AdvertPacket) == 72, "AdvertPacket must be 72 bytes");

private:
    void broadcast_loop(std::stop_token stop);
    void listen_loop(std::stop_token stop);
    void eviction_loop(std::stop_token stop);

    AdvertPacket build_advert() const;
    PeerInfo parse_advert(const AdvertPacket& pkt, const std::string& sender_addr) const;
    void notify_discovered(const PeerInfo& peer);
    void notify_lost(const PeerInfo& peer);

    NodeId node_id_;
    uint16_t port_;
    uint32_t heartbeat_interval_ms_;
    uint32_t peer_timeout_ms_;

    // Socket file descriptors
    int broadcast_fd_ = -1;
    int listen_fd_ = -1;

    // Threads
    std::jthread broadcast_thread_;
    std::jthread listen_thread_;
    std::jthread eviction_thread_;

    // Local resources (updated externally by monitor)
    mutable std::mutex resource_mutex_;
    ResourceSnapshot local_resources_;

    // Known peers with last-seen timestamp
    struct PeerRecord {
        PeerInfo info;
        std::chrono::steady_clock::time_point last_seen;
    };
    mutable std::mutex peers_mutex_;
    std::unordered_map<NodeId, PeerRecord> known_peers_;

    // Callbacks
    std::mutex callback_mutex_;
    std::vector<PeerCallback> on_discovered_;
    std::vector<PeerCallback> on_lost_;
};

}  // namespace edge_orchestrator
