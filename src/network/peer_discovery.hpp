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
    /**
     * @brief Construct a discovery agent for this node.
     *
     * @param node_id                Identity advertised to peers (max 31 chars
     *                               on the wire; longer ids are truncated).
     * @param tcp_port               The node's offload/submission TCP port.
     *                               Travels inside the advert so peers know
     *                               where to connect; discovery itself never
     *                               uses it.
     * @param discovery_port         UDP port heartbeats are broadcast to and
     *                               received on. All nodes of one cluster must
     *                               agree on it; several daemons on one host
     *                               can share it (SO_REUSEADDR — broadcast
     *                               datagrams are delivered to every bound
     *                               socket), which is what makes single-machine
     *                               multi-node clusters work.
     * @param heartbeat_interval_ms  Advert period.
     * @param peer_timeout_ms        Silence after which a peer is evicted.
     */
    PeerDiscovery(NodeId node_id,
                  uint16_t tcp_port,
                  uint16_t discovery_port,
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
     * high-frequency discovery path. 72 bytes total.
     *
     * All multi-byte fields are big-endian on the wire (the _be suffix);
     * floats travel as their IEEE-754 bit pattern in a uint32_t. Protocol
     * version 2 introduced the fixed byte order — v1 peers sent host order
     * and are rejected by the version check rather than misparsed.
     */
    struct __attribute__((packed)) AdvertPacket {
        char magic[4];             // "EORC"
        uint8_t version;           // Protocol version (2)
        uint8_t reserved[3];
        char node_id[32];          // Null-padded node identifier
        uint16_t tcp_port_be;      // Port for TCP transport
        uint16_t padding;
        uint32_t cpu_usage_bits_be;        // IEEE-754 bits of cpu percent
        uint64_t memory_available_be;
        uint64_t memory_total_be;
        uint32_t temperature_bits_be;      // IEEE-754 bits of degrees C
        uint32_t flags_be;         // Bit 0: is_throttled
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
    uint16_t tcp_port_;        ///< Advertised in packets; not bound here
    uint16_t discovery_port_;  ///< UDP port for broadcast + listen
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
