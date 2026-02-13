/**
 * @file peer_discovery.cpp
 * @brief Full PeerDiscovery implementation using POSIX UDP sockets.
 * @author Dimitris Kafetzis
 *
 * Uses SO_BROADCAST for heartbeat advertisements on the local subnet.
 * Listens for peer adverts on the same port. A separate eviction thread
 * removes peers that haven't been heard from within the timeout.
 */

#include "network/peer_discovery.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace edge_orchestrator {

namespace {

constexpr char MAGIC[4] = {'E', 'O', 'R', 'C'};
constexpr uint8_t PROTOCOL_VERSION = 1;

/**
 * @brief Create a non-blocking UDP socket with SO_BROADCAST and SO_REUSEADDR.
 */
int create_udp_socket(bool enable_broadcast) {
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;

    int optval = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    if (enable_broadcast) {
        ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval));
    }

    return fd;
}

}  // anonymous namespace

// ─────────────────────────────────────────────
// Construction / Destruction
// ─────────────────────────────────────────────

PeerDiscovery::PeerDiscovery(NodeId node_id,
                              uint16_t port,
                              uint32_t heartbeat_interval_ms,
                              uint32_t peer_timeout_ms)
    : node_id_(std::move(node_id))
    , port_(port)
    , heartbeat_interval_ms_(heartbeat_interval_ms)
    , peer_timeout_ms_(peer_timeout_ms) {
    local_resources_.node_id = node_id_;
}

PeerDiscovery::~PeerDiscovery() {
    stop();
}

// ─────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────

void PeerDiscovery::start() {
    // Create broadcast socket
    broadcast_fd_ = create_udp_socket(true);
    if (broadcast_fd_ < 0) return;

    // Create listening socket and bind
    listen_fd_ = create_udp_socket(false);
    if (listen_fd_ < 0) {
        ::close(broadcast_fd_);
        broadcast_fd_ = -1;
        return;
    }

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port_);
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
        ::close(broadcast_fd_);
        ::close(listen_fd_);
        broadcast_fd_ = listen_fd_ = -1;
        return;
    }

    // Launch threads
    broadcast_thread_ = std::jthread([this](std::stop_token stop) {
        broadcast_loop(stop);
    });
    listen_thread_ = std::jthread([this](std::stop_token stop) {
        listen_loop(stop);
    });
    eviction_thread_ = std::jthread([this](std::stop_token stop) {
        eviction_loop(stop);
    });
}

void PeerDiscovery::stop() {
    // Request stop on all threads
    if (broadcast_thread_.joinable()) broadcast_thread_.request_stop();
    if (listen_thread_.joinable()) listen_thread_.request_stop();
    if (eviction_thread_.joinable()) eviction_thread_.request_stop();

    // Close sockets to unblock any pending recvfrom
    if (broadcast_fd_ >= 0) { ::close(broadcast_fd_); broadcast_fd_ = -1; }
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
}

// ─────────────────────────────────────────────
// Callbacks and Updates
// ─────────────────────────────────────────────

void PeerDiscovery::on_peer_discovered(PeerCallback callback) {
    std::lock_guard lock(callback_mutex_);
    on_discovered_.push_back(std::move(callback));
}

void PeerDiscovery::on_peer_lost(PeerCallback callback) {
    std::lock_guard lock(callback_mutex_);
    on_lost_.push_back(std::move(callback));
}

void PeerDiscovery::update_local_resources(const ResourceSnapshot& snapshot) {
    std::lock_guard lock(resource_mutex_);
    local_resources_ = snapshot;
}

size_t PeerDiscovery::known_peer_count() const {
    std::lock_guard lock(peers_mutex_);
    return known_peers_.size();
}

// ─────────────────────────────────────────────
// Broadcast Thread
// ─────────────────────────────────────────────

void PeerDiscovery::broadcast_loop(std::stop_token stop) {
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port_);
    dest.sin_addr.s_addr = INADDR_BROADCAST;

    while (!stop.stop_requested()) {
        auto pkt = build_advert();

        if (broadcast_fd_ >= 0) {
            ::sendto(broadcast_fd_, &pkt, sizeof(pkt), 0,
                     reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        }

        // Sleep in small increments to respond to stop requests promptly
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(heartbeat_interval_ms_);
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

// ─────────────────────────────────────────────
// Listen Thread
// ─────────────────────────────────────────────

void PeerDiscovery::listen_loop(std::stop_token stop) {
    AdvertPacket pkt{};

    while (!stop.stop_requested()) {
        if (listen_fd_ < 0) break;

        // Use poll with short timeout to check stop_requested
        pollfd pfd{};
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;

        int ready = ::poll(&pfd, 1, 100);  // 100ms timeout
        if (ready <= 0) continue;

        sockaddr_in sender_addr{};
        socklen_t addr_len = sizeof(sender_addr);

        auto bytes_read = ::recvfrom(listen_fd_, &pkt, sizeof(pkt), 0,
                                     reinterpret_cast<sockaddr*>(&sender_addr), &addr_len);

        if (bytes_read != sizeof(pkt)) continue;

        // Validate magic
        if (std::memcmp(pkt.magic, MAGIC, 4) != 0) continue;
        if (pkt.version != PROTOCOL_VERSION) continue;

        // Extract sender IP
        char ip_buf[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &sender_addr.sin_addr, ip_buf, sizeof(ip_buf));
        std::string sender_ip(ip_buf);

        // Parse into PeerInfo
        auto peer = parse_advert(pkt, sender_ip);

        // Ignore our own broadcasts
        if (peer.node_id == node_id_) continue;

        // Update or insert peer
        bool is_new = false;
        {
            std::lock_guard lock(peers_mutex_);
            auto it = known_peers_.find(peer.node_id);
            if (it == known_peers_.end()) {
                is_new = true;
                known_peers_[peer.node_id] = PeerRecord{
                    .info = peer,
                    .last_seen = std::chrono::steady_clock::now()
                };
            } else {
                it->second.info = peer;
                it->second.last_seen = std::chrono::steady_clock::now();
            }
        }

        if (is_new) {
            notify_discovered(peer);
        }
    }
}

// ─────────────────────────────────────────────
// Eviction Thread
// ─────────────────────────────────────────────

void PeerDiscovery::eviction_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        auto now = std::chrono::steady_clock::now();
        auto timeout = std::chrono::milliseconds(peer_timeout_ms_);

        std::vector<PeerInfo> evicted;

        {
            std::lock_guard lock(peers_mutex_);
            for (auto it = known_peers_.begin(); it != known_peers_.end(); ) {
                if ((now - it->second.last_seen) > timeout) {
                    evicted.push_back(it->second.info);
                    it = known_peers_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (const auto& peer : evicted) {
            notify_lost(peer);
        }

        // Check every half-timeout
        auto sleep_time = std::chrono::milliseconds(peer_timeout_ms_ / 2);
        auto deadline = std::chrono::steady_clock::now() + sleep_time;
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

// ─────────────────────────────────────────────
// Packet Serialization
// ─────────────────────────────────────────────

PeerDiscovery::AdvertPacket PeerDiscovery::build_advert() const {
    AdvertPacket pkt{};
    std::memcpy(pkt.magic, MAGIC, 4);
    pkt.version = PROTOCOL_VERSION;

    // Copy node_id, null-padded
    std::memset(pkt.node_id, 0, sizeof(pkt.node_id));
    auto copy_len = std::min(node_id_.size(), sizeof(pkt.node_id) - 1);
    std::memcpy(pkt.node_id, node_id_.data(), copy_len);

    pkt.tcp_port = port_;

    // Snapshot local resources
    {
        std::lock_guard lock(resource_mutex_);
        pkt.cpu_usage_percent = local_resources_.cpu_usage_percent;
        pkt.memory_available_bytes = local_resources_.memory_available_bytes;
        pkt.memory_total_bytes = local_resources_.memory_total_bytes;
        pkt.temperature_celsius = local_resources_.cpu_temperature_celsius;
        pkt.flags = local_resources_.is_throttled ? 1u : 0u;
    }

    return pkt;
}

PeerInfo PeerDiscovery::parse_advert(const AdvertPacket& pkt,
                                      const std::string& sender_addr) const {
    PeerInfo peer;
    peer.node_id = std::string(pkt.node_id,
        strnlen(pkt.node_id, sizeof(pkt.node_id)));
    peer.reachable = true;

    peer.resources.node_id = peer.node_id;
    peer.resources.cpu_usage_percent = pkt.cpu_usage_percent;
    peer.resources.memory_available_bytes = pkt.memory_available_bytes;
    peer.resources.memory_total_bytes = pkt.memory_total_bytes;
    peer.resources.cpu_temperature_celsius = pkt.temperature_celsius;
    peer.resources.is_throttled = (pkt.flags & 1u) != 0;
    peer.resources.timestamp = std::chrono::system_clock::now();

    // Store sender address (unused in PeerInfo but useful for transport layer)
    (void)sender_addr;

    return peer;
}

void PeerDiscovery::notify_discovered(const PeerInfo& peer) {
    std::lock_guard lock(callback_mutex_);
    for (const auto& cb : on_discovered_) {
        cb(peer);
    }
}

void PeerDiscovery::notify_lost(const PeerInfo& peer) {
    std::lock_guard lock(callback_mutex_);
    for (const auto& cb : on_lost_) {
        cb(peer);
    }
}

}  // namespace edge_orchestrator
