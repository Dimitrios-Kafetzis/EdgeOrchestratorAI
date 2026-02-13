/**
 * @file test_network.cpp
 * @brief Unit tests for ClusterViewManager, TcpTransport, and PeerDiscovery.
 * @author Dimitris Kafetzis
 */

#include "network/cluster_view.hpp"
#include "network/peer_discovery.hpp"
#include "network/transport.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include <thread>

using namespace edge_orchestrator;

namespace {

ResourceSnapshot make_snap(const std::string& node_id,
                            float cpu = 30.0f,
                            uint64_t mem_avail = 2ULL * 1024 * 1024 * 1024,
                            uint64_t mem_total = 4ULL * 1024 * 1024 * 1024) {
    ResourceSnapshot snap;
    snap.node_id = node_id;
    snap.cpu_usage_percent = cpu;
    snap.memory_available_bytes = mem_avail;
    snap.memory_total_bytes = mem_total;
    snap.timestamp = std::chrono::system_clock::now();
    return snap;
}

}  // namespace

// ═══════════════════════════════════════════════
// ClusterViewManager Tests
// ═══════════════════════════════════════════════

TEST(ClusterViewManagerTest, EmptyByDefault) {
    ClusterViewManager mgr;
    EXPECT_EQ(mgr.cluster_size(), 0u);
    EXPECT_TRUE(mgr.available_peers().empty());
    EXPECT_TRUE(mgr.snapshot().peers.empty());
}

TEST(ClusterViewManagerTest, AddAndQueryPeer) {
    ClusterViewManager mgr;
    mgr.update_peer("node-1", make_snap("node-1", 25.0f));

    EXPECT_EQ(mgr.cluster_size(), 1u);
    EXPECT_TRUE(mgr.has_peer("node-1"));
    EXPECT_FALSE(mgr.has_peer("node-2"));

    auto resources = mgr.peer_resources("node-1");
    ASSERT_TRUE(resources.has_value());
    EXPECT_FLOAT_EQ(resources->cpu_usage_percent, 25.0f);
}

TEST(ClusterViewManagerTest, UpdateExistingPeer) {
    ClusterViewManager mgr;
    mgr.update_peer("node-1", make_snap("node-1", 25.0f));
    mgr.update_peer("node-1", make_snap("node-1", 80.0f));

    EXPECT_EQ(mgr.cluster_size(), 1u);
    auto resources = mgr.peer_resources("node-1");
    ASSERT_TRUE(resources.has_value());
    EXPECT_FLOAT_EQ(resources->cpu_usage_percent, 80.0f);
}

TEST(ClusterViewManagerTest, MultiplePeers) {
    ClusterViewManager mgr;
    mgr.update_peer("node-1", make_snap("node-1", 20.0f));
    mgr.update_peer("node-2", make_snap("node-2", 40.0f));
    mgr.update_peer("node-3", make_snap("node-3", 60.0f));

    EXPECT_EQ(mgr.cluster_size(), 3u);
    EXPECT_EQ(mgr.available_peers().size(), 3u);
}

TEST(ClusterViewManagerTest, RemovePeer) {
    ClusterViewManager mgr;
    mgr.update_peer("node-1", make_snap("node-1"));
    mgr.update_peer("node-2", make_snap("node-2"));
    mgr.remove_peer("node-1");

    EXPECT_EQ(mgr.cluster_size(), 1u);
    EXPECT_FALSE(mgr.has_peer("node-1"));
    EXPECT_TRUE(mgr.has_peer("node-2"));
}

TEST(ClusterViewManagerTest, MarkUnreachable) {
    ClusterViewManager mgr;
    mgr.update_peer("node-1", make_snap("node-1"));
    mgr.update_peer("node-2", make_snap("node-2"));
    mgr.mark_unreachable("node-1");

    // Still in the map but not reachable
    EXPECT_EQ(mgr.cluster_size(), 2u);
    EXPECT_TRUE(mgr.has_peer("node-1"));

    // Not in available peers or snapshot
    EXPECT_EQ(mgr.available_peers().size(), 1u);
    EXPECT_EQ(mgr.snapshot().peers.size(), 1u);

    // peer_resources returns nullopt for unreachable
    EXPECT_FALSE(mgr.peer_resources("node-1").has_value());
}

TEST(ClusterViewManagerTest, Clear) {
    ClusterViewManager mgr;
    mgr.update_peer("node-1", make_snap("node-1"));
    mgr.update_peer("node-2", make_snap("node-2"));
    mgr.clear();

    EXPECT_EQ(mgr.cluster_size(), 0u);
    EXPECT_TRUE(mgr.available_peers().empty());
}

TEST(ClusterViewManagerTest, SnapshotIncludesOnlyReachable) {
    ClusterViewManager mgr;
    mgr.update_peer("node-1", make_snap("node-1"));
    mgr.update_peer("node-2", make_snap("node-2"));
    mgr.update_peer("node-3", make_snap("node-3"));
    mgr.mark_unreachable("node-2");

    auto view = mgr.snapshot();
    EXPECT_EQ(view.peers.size(), 2u);
    EXPECT_EQ(view.available_count(), 2u);
}

TEST(ClusterViewManagerTest, ReachAfterUnreachable) {
    ClusterViewManager mgr;
    mgr.update_peer("node-1", make_snap("node-1"));
    mgr.mark_unreachable("node-1");
    EXPECT_EQ(mgr.snapshot().peers.size(), 0u);

    // Re-updating makes it reachable again
    mgr.update_peer("node-1", make_snap("node-1"));
    EXPECT_EQ(mgr.snapshot().peers.size(), 1u);
}

TEST(ClusterViewManagerTest, ThreadSafety) {
    ClusterViewManager mgr;

    // Concurrent writers
    std::vector<std::thread> writers;
    for (int i = 0; i < 10; ++i) {
        writers.emplace_back([&mgr, i]() {
            for (int j = 0; j < 100; ++j) {
                auto id = "node-" + std::to_string(i);
                mgr.update_peer(id, make_snap(id, static_cast<float>(j)));
            }
        });
    }

    // Concurrent readers
    std::vector<std::thread> readers;
    for (int i = 0; i < 5; ++i) {
        readers.emplace_back([&mgr]() {
            for (int j = 0; j < 200; ++j) {
                auto view = mgr.snapshot();
                auto peers = mgr.available_peers();
                auto size = mgr.cluster_size();
                (void)view;
                (void)peers;
                (void)size;
            }
        });
    }

    for (auto& t : writers) t.join();
    for (auto& t : readers) t.join();

    EXPECT_EQ(mgr.cluster_size(), 10u);
}

// ═══════════════════════════════════════════════
// TcpTransport Tests
// ═══════════════════════════════════════════════

TEST(TcpTransportTest, DefaultState) {
    TcpTransport transport;
    EXPECT_FALSE(transport.is_connected());
    EXPECT_FALSE(transport.is_listening());
}

TEST(TcpTransportTest, SendWithoutConnect) {
    TcpTransport transport;
    auto result = transport.send({0x01, 0x02, 0x03});
    EXPECT_FALSE(result.has_value());
}

TEST(TcpTransportTest, ReceiveWithoutConnect) {
    TcpTransport transport;
    auto result = transport.receive(100);
    EXPECT_FALSE(result.has_value());
}

TEST(TcpTransportTest, ConnectToNowhere) {
    TcpTransport transport;
    // Connect to a port that's not listening — should fail
    auto result = transport.connect("127.0.0.1", 59999, 500);
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(transport.is_connected());
}

TEST(TcpTransportTest, ListenAndConnect) {
    TcpTransport server;
    auto listen_result = server.listen(0);  // Port 0 = OS-assigned
    // Note: port 0 won't work for connecting back, use a fixed port
    server.stop_serving();
}

TEST(TcpTransportTest, RoundTrip) {
    // Server
    TcpTransport server;
    uint16_t port = 19876;
    auto listen_result = server.listen(port);
    if (!listen_result.has_value()) {
        // Port might be in use — skip test
        GTEST_SKIP() << "Could not bind to port " << port;
    }

    // Server echoes back the received message with "ECHO:" prefix
    server.serve([](const std::vector<uint8_t>& request) -> std::vector<uint8_t> {
        std::vector<uint8_t> response;
        response.push_back('E');
        response.push_back('C');
        response.push_back('H');
        response.push_back('O');
        response.push_back(':');
        response.insert(response.end(), request.begin(), request.end());
        return response;
    });

    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Client
    TcpTransport client;
    auto connect_result = client.connect("127.0.0.1", port, 2000);
    ASSERT_TRUE(connect_result.has_value()) << connect_result.error().message;
    EXPECT_TRUE(client.is_connected());

    // Send a message
    std::vector<uint8_t> message = {'H', 'E', 'L', 'L', 'O'};
    auto send_result = client.send(message);
    ASSERT_TRUE(send_result.has_value()) << send_result.error().message;

    // Receive response
    auto recv_result = client.receive(5000);
    ASSERT_TRUE(recv_result.has_value()) << recv_result.error().message;

    auto& response = *recv_result;
    ASSERT_EQ(response.size(), 10u);  // "ECHO:" + "HELLO"
    EXPECT_EQ(response[0], 'E');
    EXPECT_EQ(response[1], 'C');
    EXPECT_EQ(response[4], ':');
    EXPECT_EQ(response[5], 'H');
    EXPECT_EQ(response[9], 'O');

    client.disconnect();
    EXPECT_FALSE(client.is_connected());
    server.stop_serving();
}

TEST(TcpTransportTest, LargeMessage) {
    TcpTransport server;
    uint16_t port = 19877;
    auto listen_result = server.listen(port);
    if (!listen_result.has_value()) {
        GTEST_SKIP() << "Could not bind to port " << port;
    }

    // Echo server
    server.serve([](const std::vector<uint8_t>& request) {
        return request;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TcpTransport client;
    auto connect_result = client.connect("127.0.0.1", port, 2000);
    ASSERT_TRUE(connect_result.has_value());

    // Send 1 MB message
    std::vector<uint8_t> large_msg(1024 * 1024);
    for (size_t i = 0; i < large_msg.size(); ++i) {
        large_msg[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto send_result = client.send(large_msg);
    ASSERT_TRUE(send_result.has_value());

    auto recv_result = client.receive(10000);
    ASSERT_TRUE(recv_result.has_value());
    EXPECT_EQ(recv_result->size(), large_msg.size());
    EXPECT_EQ(*recv_result, large_msg);

    client.disconnect();
    server.stop_serving();
}

TEST(TcpTransportTest, EmptyMessage) {
    TcpTransport server;
    uint16_t port = 19878;
    auto listen_result = server.listen(port);
    if (!listen_result.has_value()) {
        GTEST_SKIP() << "Could not bind to port " << port;
    }

    server.serve([](const std::vector<uint8_t>& request) {
        return request;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TcpTransport client;
    auto connect_result = client.connect("127.0.0.1", port, 2000);
    ASSERT_TRUE(connect_result.has_value());

    std::vector<uint8_t> empty_msg;
    auto send_result = client.send(empty_msg);
    ASSERT_TRUE(send_result.has_value());

    auto recv_result = client.receive(5000);
    ASSERT_TRUE(recv_result.has_value());
    EXPECT_TRUE(recv_result->empty());

    client.disconnect();
    server.stop_serving();
}

TEST(TcpTransportTest, DisconnectThenReconnect) {
    TcpTransport server;
    uint16_t port = 19879;
    auto listen_result = server.listen(port);
    if (!listen_result.has_value()) {
        GTEST_SKIP() << "Could not bind to port " << port;
    }

    server.serve([](const std::vector<uint8_t>& req) { return req; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TcpTransport client;

    // First connection
    ASSERT_TRUE(client.connect("127.0.0.1", port, 2000).has_value());
    client.disconnect();
    EXPECT_FALSE(client.is_connected());

    // Reconnect
    ASSERT_TRUE(client.connect("127.0.0.1", port, 2000).has_value());
    EXPECT_TRUE(client.is_connected());

    client.disconnect();
    server.stop_serving();
}

// ═══════════════════════════════════════════════
// PeerDiscovery Tests
// ═══════════════════════════════════════════════

TEST(PeerDiscoveryTest, Construction) {
    PeerDiscovery discovery("test-node", 15200, 1000, 3000);
    EXPECT_EQ(discovery.known_peer_count(), 0u);
}

TEST(PeerDiscoveryTest, AdvertPacketSize) {
    EXPECT_EQ(sizeof(PeerDiscovery::AdvertPacket), 72u);
}

TEST(PeerDiscoveryTest, UpdateLocalResources) {
    PeerDiscovery discovery("test-node", 15200, 1000, 3000);

    ResourceSnapshot snap;
    snap.cpu_usage_percent = 75.0f;
    snap.memory_available_bytes = 1ULL * 1024 * 1024 * 1024;
    snap.memory_total_bytes = 4ULL * 1024 * 1024 * 1024;

    // Should not crash
    discovery.update_local_resources(snap);
}

TEST(PeerDiscoveryTest, CallbackRegistration) {
    PeerDiscovery discovery("test-node", 15200, 1000, 3000);

    bool discovered = false;
    bool lost = false;

    discovery.on_peer_discovered([&](const PeerInfo&) { discovered = true; });
    discovery.on_peer_lost([&](const PeerInfo&) { lost = true; });

    // Callbacks are registered but not fired without start()
    EXPECT_FALSE(discovered);
    EXPECT_FALSE(lost);
}

TEST(PeerDiscoveryTest, SelfDiscoveryFiltered) {
    // Two discoveries on the same port — they should hear each other
    // but filter their own broadcasts
    PeerDiscovery disc1("node-A", 15201, 200, 2000);
    PeerDiscovery disc2("node-B", 15201, 200, 2000);

    bool a_found_b = false;
    bool b_found_a = false;
    bool a_found_self = false;

    disc1.on_peer_discovered([&](const PeerInfo& peer) {
        if (peer.node_id == "node-B") a_found_b = true;
        if (peer.node_id == "node-A") a_found_self = true;
    });

    disc2.on_peer_discovered([&](const PeerInfo& peer) {
        if (peer.node_id == "node-A") b_found_a = true;
    });

    disc1.start();
    disc2.start();

    // Wait for at least one heartbeat cycle
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    disc1.stop();
    disc2.stop();

    // Both should discover each other
    EXPECT_TRUE(a_found_b) << "Node A should have discovered Node B";
    EXPECT_TRUE(b_found_a) << "Node B should have discovered Node A";

    // Neither should discover itself
    EXPECT_FALSE(a_found_self) << "Node A should not discover itself";
}

TEST(PeerDiscoveryTest, PeerEviction) {
    // disc1 broadcasts, disc2 listens. When disc1 stops, disc2 should
    // eventually evict it.
    PeerDiscovery disc1("node-fast", 15202, 100, 500);
    PeerDiscovery disc2("node-watcher", 15202, 100, 400);  // Short timeout

    bool peer_lost = false;
    std::string lost_id;

    disc2.on_peer_lost([&](const PeerInfo& peer) {
        peer_lost = true;
        lost_id = peer.node_id;
    });

    disc1.start();
    disc2.start();

    // Let them discover each other
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_GT(disc2.known_peer_count(), 0u);

    // Stop disc1 — disc2 should evict it after timeout
    disc1.stop();

    // Wait longer than the peer timeout + eviction interval
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    disc2.stop();

    EXPECT_TRUE(peer_lost) << "Watcher should have evicted the stopped peer";
    EXPECT_EQ(lost_id, "node-fast");
    EXPECT_EQ(disc2.known_peer_count(), 0u);
}

TEST(PeerDiscoveryTest, ResourcesInAdvertisement) {
    PeerDiscovery disc1("node-sender", 15203, 200, 2000);
    PeerDiscovery disc2("node-receiver", 15203, 200, 2000);

    // Set specific resource values on disc1
    ResourceSnapshot snap;
    snap.cpu_usage_percent = 42.5f;
    snap.memory_available_bytes = 1234567890ULL;
    snap.memory_total_bytes = 4ULL * 1024 * 1024 * 1024;
    snap.cpu_temperature_celsius = 55.0f;
    snap.is_throttled = true;
    disc1.update_local_resources(snap);

    float received_cpu = 0.0f;
    uint64_t received_mem = 0;
    bool received_throttled = false;

    disc2.on_peer_discovered([&](const PeerInfo& peer) {
        if (peer.node_id == "node-sender") {
            received_cpu = peer.resources.cpu_usage_percent;
            received_mem = peer.resources.memory_available_bytes;
            received_throttled = peer.resources.is_throttled;
        }
    });

    disc1.start();
    disc2.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    disc1.stop();
    disc2.stop();

    // Verify resource values were transmitted correctly
    EXPECT_FLOAT_EQ(received_cpu, 42.5f);
    EXPECT_EQ(received_mem, 1234567890ULL);
    EXPECT_TRUE(received_throttled);
}
