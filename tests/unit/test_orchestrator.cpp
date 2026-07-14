/**
 * @file test_orchestrator.cpp
 * @brief Tests for Orchestrator facade and OffloadCodec.
 * @author Dimitris Kafetzis
 */

#include "orchestrator/orchestrator.hpp"
#include "resource_monitor/monitor.hpp"
#include "workload/generator.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace edge_orchestrator;

// ═══════════════════════════════════════════════
// OffloadCodec Tests
// ═══════════════════════════════════════════════

TEST(OffloadCodecTest, RequestRoundTrip) {
    TaskProfile profile{
        .compute_cost = Duration{5000},
        .memory_bytes = 1024 * 1024,
        .input_bytes = 4096,
        .output_bytes = 2048
    };
    std::vector<uint8_t> input = {0xDE, 0xAD, 0xBE, 0xEF};

    auto encoded = OffloadCodec::encode_request("task-42", profile, input);
    EXPECT_GT(encoded.size(), 0u);

    std::string task_id;
    TaskProfile decoded_profile;
    std::vector<uint8_t> decoded_input;

    ASSERT_TRUE(OffloadCodec::decode_request(encoded, task_id, decoded_profile, decoded_input));
    EXPECT_EQ(task_id, "task-42");
    EXPECT_EQ(decoded_profile.compute_cost.count(), 5000);
    EXPECT_EQ(decoded_profile.memory_bytes, 1024u * 1024);
    EXPECT_EQ(decoded_profile.input_bytes, 4096u);
    EXPECT_EQ(decoded_profile.output_bytes, 2048u);
    EXPECT_EQ(decoded_input, input);
}

TEST(OffloadCodecTest, RequestWithEmptyInput) {
    TaskProfile profile{.compute_cost = Duration{100}, .memory_bytes = 256};
    auto encoded = OffloadCodec::encode_request("t-1", profile);

    std::string task_id;
    TaskProfile decoded;
    std::vector<uint8_t> input;

    ASSERT_TRUE(OffloadCodec::decode_request(encoded, task_id, decoded, input));
    EXPECT_EQ(task_id, "t-1");
    EXPECT_TRUE(input.empty());
}

TEST(OffloadCodecTest, RequestDecodeTooShort) {
    std::vector<uint8_t> bad_data = {0x01, 0x02};
    std::string id;
    TaskProfile p;
    std::vector<uint8_t> input;
    EXPECT_FALSE(OffloadCodec::decode_request(bad_data, id, p, input));
}

TEST(OffloadCodecTest, SuccessResponseRoundTrip) {
    auto encoded = OffloadCodec::encode_response(true, Duration{3500}, 65536);

    bool success;
    Duration dur;
    uint64_t peak_mem;
    std::string err;
    std::vector<uint8_t> output;

    ASSERT_TRUE(OffloadCodec::decode_response(encoded, success, dur, peak_mem, err, output));
    EXPECT_TRUE(success);
    EXPECT_EQ(dur.count(), 3500);
    EXPECT_EQ(peak_mem, 65536u);
    EXPECT_TRUE(err.empty());
    EXPECT_TRUE(output.empty());
}

TEST(OffloadCodecTest, ErrorResponseRoundTrip) {
    auto encoded = OffloadCodec::encode_response(false, Duration{0}, 0, "out of memory");

    bool success;
    Duration dur;
    uint64_t peak_mem;
    std::string err;
    std::vector<uint8_t> output;

    ASSERT_TRUE(OffloadCodec::decode_response(encoded, success, dur, peak_mem, err, output));
    EXPECT_FALSE(success);
    EXPECT_EQ(err, "out of memory");
}

TEST(OffloadCodecTest, ResponseWithOutput) {
    std::vector<uint8_t> out_data = {0x01, 0x02, 0x03, 0x04, 0x05};
    auto encoded = OffloadCodec::encode_response(true, Duration{1000}, 512, "", out_data);

    bool success;
    Duration dur;
    uint64_t peak_mem;
    std::string err;
    std::vector<uint8_t> output;

    ASSERT_TRUE(OffloadCodec::decode_response(encoded, success, dur, peak_mem, err, output));
    EXPECT_TRUE(success);
    EXPECT_EQ(output, out_data);
}

TEST(OffloadCodecTest, ResponseDecodeTooShort) {
    std::vector<uint8_t> bad_data = {0x00};
    bool s;
    Duration d;
    uint64_t pm;
    std::string e;
    std::vector<uint8_t> o;
    EXPECT_FALSE(OffloadCodec::decode_response(bad_data, s, d, pm, e, o));
}

TEST(OffloadCodecTest, LargeTaskId) {
    std::string long_id(200, 'x');
    TaskProfile profile{.compute_cost = Duration{100}, .memory_bytes = 256};

    auto encoded = OffloadCodec::encode_request(long_id, profile);

    std::string decoded_id;
    TaskProfile decoded;
    std::vector<uint8_t> input;
    ASSERT_TRUE(OffloadCodec::decode_request(encoded, decoded_id, decoded, input));
    EXPECT_EQ(decoded_id, long_id);
}

// ═══════════════════════════════════════════════
// TCP Offload E2E with Codec
// ═══════════════════════════════════════════════

TEST(OffloadE2ETest, TcpOffloadWithCodec) {
    // Server that decodes request, "executes" task, returns response
    TcpTransport server;
    uint16_t port = 19960;
    auto listen_result = server.listen(port);
    if (!listen_result.has_value()) {
        GTEST_SKIP() << "Could not bind to port " << port;
    }

    server.serve([](const std::vector<uint8_t>& request) -> std::vector<uint8_t> {
        std::string task_id;
        TaskProfile profile;
        std::vector<uint8_t> input;

        if (!OffloadCodec::decode_request(request, task_id, profile, input)) {
            return OffloadCodec::encode_response(false, Duration{0}, 0, "decode failed");
        }

        // Simulate execution
        return OffloadCodec::encode_response(true, profile.compute_cost,
                                              profile.memory_bytes);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Client sends offload request
    TcpTransport client;
    ASSERT_TRUE(client.connect("127.0.0.1", port, 2000).has_value());

    TaskProfile profile{.compute_cost = Duration{2000}, .memory_bytes = 8192};
    auto request = OffloadCodec::encode_request("offloaded-task-1", profile);
    ASSERT_TRUE(client.send(request).has_value());

    auto recv_result = client.receive(5000);
    ASSERT_TRUE(recv_result.has_value());

    bool success;
    Duration dur;
    uint64_t peak_mem;
    std::string err;
    std::vector<uint8_t> output;
    ASSERT_TRUE(OffloadCodec::decode_response(*recv_result, success, dur, peak_mem, err, output));

    EXPECT_TRUE(success);
    EXPECT_EQ(dur.count(), 2000);
    EXPECT_EQ(peak_mem, 8192u);

    client.disconnect();
    server.stop_serving();
}

// The orchestrator itself resolves the peer endpoint from the cluster view,
// connects, and round-trips the task — no test-side client involved.
TEST(OffloadE2ETest, OrchestratorOffloadsToRealPeer) {
    // Fake peer: decodes each request, "executes", responds, and counts.
    TcpTransport peer_server;
    uint16_t peer_port = 19961;
    auto listen_result = peer_server.listen(peer_port);
    if (!listen_result.has_value()) {
        GTEST_SKIP() << "Could not bind to port " << peer_port;
    }

    std::atomic<size_t> remote_requests{0};
    peer_server.serve([&remote_requests](const std::vector<uint8_t>& request) {
        std::string task_id;
        TaskProfile profile;
        std::vector<uint8_t> input;
        if (!OffloadCodec::decode_request(request, task_id, profile, input)) {
            return OffloadCodec::encode_response(false, Duration{0}, 0, "decode failed");
        }
        remote_requests.fetch_add(1);
        return OffloadCodec::encode_response(true, profile.compute_cost,
                                             profile.memory_bytes);
    });

    auto config = default_config();
    config.node.id = "offload-origin";
    config.node.port = 19962;
    config.executor.thread_count = 2;
    config.executor.memory_pool_mb = 4;
    config.scheduler.policy = "greedy";

    Orchestrator<MockMonitor> orch({
        .config = config,
        .log_sink = std::make_unique<NullSink>(),
        .log_level = LogLevel::Debug
    });

    // Local node is busy; the injected peer is idle, so greedy offloads.
    orch.monitor().set_cpu(95.0f);
    orch.monitor().set_memory(256ULL * 1024 * 1024, 4ULL * 1024 * 1024 * 1024);
    ASSERT_TRUE(orch.start().has_value());

    PeerInfo peer;
    peer.node_id = "idle-peer";
    peer.address = "127.0.0.1";
    peer.tcp_port = peer_port;
    peer.resources.node_id = "idle-peer";
    peer.resources.cpu_usage_percent = 5.0f;
    peer.resources.memory_available_bytes = 3ULL * 1024 * 1024 * 1024;
    peer.resources.memory_total_bytes = 4ULL * 1024 * 1024 * 1024;
    orch.cluster().update_peer(peer);

    TaskProfile profile{.compute_cost = Duration{1000}, .memory_bytes = 4096};
    auto dag = WorkloadGenerator::linear_chain(4, profile);
    auto result = orch.submit_workload(dag);
    ASSERT_TRUE(result.has_value());

    EXPECT_GT(result->offloaded_tasks, 0u);
    EXPECT_EQ(result->offload_fallbacks, 0u);
    EXPECT_EQ(result->completed_tasks, dag.task_count());
    EXPECT_EQ(result->failed_tasks, 0u);
    EXPECT_EQ(remote_requests.load(), result->offloaded_tasks);

    orch.stop();
    peer_server.stop_serving();
}

// A peer that dies after being scheduled against must not lose tasks:
// the offload fails over to local execution.
TEST(OffloadE2ETest, OffloadFallsBackWhenPeerUnreachable) {
    auto config = default_config();
    config.node.id = "fallback-origin";
    config.node.port = 19963;
    config.executor.thread_count = 2;
    config.executor.memory_pool_mb = 4;
    config.scheduler.policy = "greedy";

    Orchestrator<MockMonitor> orch({
        .config = config,
        .log_sink = std::make_unique<NullSink>(),
        .log_level = LogLevel::Debug
    });

    orch.monitor().set_cpu(95.0f);
    orch.monitor().set_memory(256ULL * 1024 * 1024, 4ULL * 1024 * 1024 * 1024);
    ASSERT_TRUE(orch.start().has_value());

    // Endpoint points at a closed port: connect gets an immediate refusal.
    PeerInfo peer;
    peer.node_id = "dead-peer";
    peer.address = "127.0.0.1";
    peer.tcp_port = 19964;
    peer.resources.node_id = "dead-peer";
    peer.resources.cpu_usage_percent = 5.0f;
    peer.resources.memory_available_bytes = 3ULL * 1024 * 1024 * 1024;
    peer.resources.memory_total_bytes = 4ULL * 1024 * 1024 * 1024;
    orch.cluster().update_peer(peer);

    TaskProfile profile{.compute_cost = Duration{1000}, .memory_bytes = 4096};
    auto dag = WorkloadGenerator::linear_chain(4, profile);
    auto result = orch.submit_workload(dag);
    ASSERT_TRUE(result.has_value());

    EXPECT_GT(result->offloaded_tasks, 0u);
    EXPECT_EQ(result->offload_fallbacks, result->offloaded_tasks);
    EXPECT_EQ(result->completed_tasks, dag.task_count());
    EXPECT_EQ(result->failed_tasks, 0u);

    orch.stop();
}

// ═══════════════════════════════════════════════
// Workload Submission
// ═══════════════════════════════════════════════

TEST(WorkloadSubmissionTest, CodecRoundTrip) {
    TaskProfile profile{.compute_cost = Duration{5000}, .memory_bytes = 65536,
                        .input_bytes = 1024, .output_bytes = 2048};
    auto dag = WorkloadGenerator::diamond(2, 3, profile);

    auto encoded = OffloadCodec::encode_submission("wl-7", dag);
    EXPECT_EQ(OffloadCodec::classify(encoded),
              OffloadCodec::WireKind::WorkloadSubmission);

    std::string workload_id;
    WorkloadDAG decoded;
    ASSERT_TRUE(OffloadCodec::decode_submission(encoded, workload_id, decoded));

    EXPECT_EQ(workload_id, "wl-7");
    EXPECT_EQ(decoded.task_count(), dag.task_count());
    EXPECT_EQ(decoded.critical_path_cost(), dag.critical_path_cost());

    for (const auto& id : dag.topological_order()) {
        auto original = dag.get_task(id);
        auto copy = decoded.get_task(id);
        ASSERT_TRUE(copy.has_value());
        EXPECT_EQ(copy->profile, original->profile);
        EXPECT_EQ(copy->dependencies, original->dependencies);
    }
}

TEST(WorkloadSubmissionTest, RejectsMalformedSubmission) {
    std::string id;
    WorkloadDAG dag;
    std::vector<uint8_t> garbage{0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_FALSE(OffloadCodec::decode_submission(garbage, id, dag));
}

// A client submits a workload over TCP and gets back an executed result —
// the same path tools/workload_injector.py uses.
TEST(WorkloadSubmissionTest, SubmitOverTcp) {
    auto config = default_config();
    config.node.id = "submission-node";
    config.node.port = 19965;
    config.executor.thread_count = 2;
    config.executor.memory_pool_mb = 4;
    config.scheduler.policy = "greedy";

    Orchestrator<MockMonitor> orch({
        .config = config,
        .log_sink = std::make_unique<NullSink>(),
        .log_level = LogLevel::Debug
    });
    orch.monitor().set_cpu(20.0f);
    orch.monitor().set_memory(3ULL * 1024 * 1024 * 1024, 4ULL * 1024 * 1024 * 1024);
    ASSERT_TRUE(orch.start().has_value());

    TaskProfile profile{.compute_cost = Duration{1000}, .memory_bytes = 4096};
    auto dag = WorkloadGenerator::linear_chain(3, profile);

    TcpTransport client;
    ASSERT_TRUE(client.connect("127.0.0.1", config.node.port, 2000).has_value());
    ASSERT_TRUE(client.send(OffloadCodec::encode_submission("wl-tcp", dag)).has_value());

    auto reply = client.receive(15000);
    ASSERT_TRUE(reply.has_value());

    std::string workload_id;
    bool accepted = false;
    std::string error;
    OrchestrationResult result;
    ASSERT_TRUE(OffloadCodec::decode_workload_result(*reply, workload_id,
                                                     accepted, error, result));
    EXPECT_EQ(workload_id, "wl-tcp");
    EXPECT_TRUE(accepted) << error;
    EXPECT_EQ(result.completed_tasks, dag.task_count());
    EXPECT_EQ(result.failed_tasks, 0u);

    client.disconnect();
    orch.stop();
}

// Regression: the arena is wholesale-reset at workload boundaries. Without
// the reset, allocations accumulate across submissions and the second
// workload here would exhaust the 4 MB pool and fail tasks.
TEST(WorkloadSubmissionTest, ArenaResetsBetweenWorkloads) {
    auto config = default_config();
    config.node.id = "arena-node";
    config.node.port = 19966;
    config.executor.thread_count = 2;
    config.executor.memory_pool_mb = 4;
    config.scheduler.policy = "greedy";

    Orchestrator<MockMonitor> orch({
        .config = config,
        .log_sink = std::make_unique<NullSink>(),
        .log_level = LogLevel::Debug
    });
    orch.monitor().set_cpu(20.0f);
    orch.monitor().set_memory(3ULL * 1024 * 1024 * 1024, 4ULL * 1024 * 1024 * 1024);
    ASSERT_TRUE(orch.start().has_value());

    // Each workload allocates ~3 MB of the 4 MB budget.
    TaskProfile profile{.compute_cost = Duration{500},
                        .memory_bytes = 1024 * 1024};
    auto dag = WorkloadGenerator::linear_chain(3, profile);

    for (int round = 0; round < 3; ++round) {
        auto result = orch.submit_workload(dag);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->completed_tasks, dag.task_count())
            << "workload round " << round << " lost tasks to arena exhaustion";
        EXPECT_EQ(result->failed_tasks, 0u);
    }

    orch.stop();
}

// ═══════════════════════════════════════════════
// Orchestrator with MockMonitor
// ═══════════════════════════════════════════════

TEST(OrchestratorTest, ConstructWithMock) {
    auto config = default_config();
    config.node.id = "test-orch";
    config.node.port = 19970;
    config.executor.thread_count = 2;
    config.executor.memory_pool_mb = 4;

    Orchestrator<MockMonitor>::Options opts{
        .config = config,
        .log_sink = std::make_unique<NullSink>(),
        .log_level = LogLevel::Debug
    };

    Orchestrator<MockMonitor> orch(std::move(opts));
    EXPECT_FALSE(orch.is_running());
}

TEST(OrchestratorTest, StartAndStop) {
    auto config = default_config();
    config.node.id = "start-stop-node";
    config.node.port = 19971;
    config.executor.thread_count = 2;
    config.executor.memory_pool_mb = 4;
    config.network.heartbeat_interval_ms = 500;

    Orchestrator<MockMonitor>::Options opts{
        .config = config,
        .log_sink = std::make_unique<NullSink>(),
        .log_level = LogLevel::Debug
    };

    Orchestrator<MockMonitor> orch(std::move(opts));
    orch.monitor().set_cpu(25.0f);
    orch.monitor().set_memory(3ULL * 1024 * 1024 * 1024, 4ULL * 1024 * 1024 * 1024);

    auto result = orch.start();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(orch.is_running());

    // Double-start should fail
    auto result2 = orch.start();
    EXPECT_FALSE(result2.has_value());

    orch.stop();
    EXPECT_FALSE(orch.is_running());
}

TEST(OrchestratorTest, SubmitLinearChain) {
    auto config = default_config();
    config.node.id = "submit-node";
    config.node.port = 19972;
    config.executor.thread_count = 4;
    config.executor.memory_pool_mb = 8;
    config.scheduler.policy = "greedy";

    Orchestrator<MockMonitor>::Options opts{
        .config = config,
        .log_sink = std::make_unique<NullSink>(),
        .log_level = LogLevel::Debug
    };

    Orchestrator<MockMonitor> orch(std::move(opts));
    orch.monitor().set_cpu(20.0f);
    orch.monitor().set_memory(3ULL * 1024 * 1024 * 1024, 4ULL * 1024 * 1024 * 1024);

    auto start_result = orch.start();
    ASSERT_TRUE(start_result.has_value());

    TaskProfile profile{.compute_cost = Duration{200}, .memory_bytes = 1024};
    auto dag = WorkloadGenerator::linear_chain(5, profile);

    auto result = orch.submit_workload(dag);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->local_tasks, 5u);
    EXPECT_EQ(result->offloaded_tasks, 0u);
    EXPECT_EQ(result->completed_tasks, 5u);
    EXPECT_EQ(result->failed_tasks, 0u);
    EXPECT_GT(result->total_duration.count(), 0);

    orch.stop();
}

TEST(OrchestratorTest, SubmitTransformerWorkload) {
    auto config = default_config();
    config.node.id = "transformer-node";
    config.node.port = 19973;
    config.executor.thread_count = 4;
    config.executor.memory_pool_mb = 16;
    config.scheduler.policy = "optimizer";
    config.scheduler.optimizer.max_iterations = 50;

    Orchestrator<MockMonitor>::Options opts{
        .config = config,
        .log_sink = std::make_unique<NullSink>(),
        .log_level = LogLevel::Debug
    };

    Orchestrator<MockMonitor> orch(std::move(opts));
    orch.monitor().set_cpu(15.0f);
    orch.monitor().set_memory(3ULL * 1024 * 1024 * 1024, 4ULL * 1024 * 1024 * 1024);

    ASSERT_TRUE(orch.start().has_value());

    auto dag = WorkloadGenerator::transformer_layers(4, 256, 512);

    auto result = orch.submit_workload(dag);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->completed_tasks + result->failed_tasks, dag.task_count());
    EXPECT_EQ(result->completed_tasks, dag.task_count());

    orch.stop();
}

TEST(OrchestratorTest, SubmitWithPeers) {
    auto config = default_config();
    config.node.id = "main-node";
    config.node.port = 19974;
    config.executor.thread_count = 4;
    config.executor.memory_pool_mb = 8;
    config.scheduler.policy = "threshold";
    config.scheduler.threshold.cpu_threshold_percent = 30.0f;

    Orchestrator<MockMonitor>::Options opts{
        .config = config,
        .log_sink = std::make_unique<NullSink>(),
        .log_level = LogLevel::Debug
    };

    Orchestrator<MockMonitor> orch(std::move(opts));
    orch.monitor().set_cpu(80.0f);  // High CPU → trigger offloading
    orch.monitor().set_memory(3ULL * 1024 * 1024 * 1024, 4ULL * 1024 * 1024 * 1024);

    // Add a simulated peer
    ResourceSnapshot peer_snap;
    peer_snap.node_id = "helper-peer";
    peer_snap.cpu_usage_percent = 10.0f;
    peer_snap.memory_available_bytes = 3ULL * 1024 * 1024 * 1024;
    peer_snap.memory_total_bytes = 4ULL * 1024 * 1024 * 1024;
    orch.cluster().update_peer("helper-peer", peer_snap);

    ASSERT_TRUE(orch.start().has_value());

    TaskProfile profile{.compute_cost = Duration{300}, .memory_bytes = 2048};
    auto dag = WorkloadGenerator::fan_out_fan_in(6, profile);

    auto result = orch.submit_workload(dag);
    ASSERT_TRUE(result.has_value());

    // With threshold at 30% and CPU at 80%, some tasks should be offloaded
    EXPECT_GT(result->offloaded_tasks, 0u) << "Should offload when CPU > threshold";
    EXPECT_EQ(result->completed_tasks, dag.task_count());

    orch.stop();
}

TEST(OrchestratorTest, PolicySelection) {
    for (const auto& policy_name : {"greedy", "threshold", "optimizer"}) {
        auto config = default_config();
        config.node.id = "policy-test";
        config.node.port = 19975;
        config.scheduler.policy = policy_name;
        config.executor.thread_count = 2;
        config.executor.memory_pool_mb = 4;

        Orchestrator<MockMonitor>::Options opts{
            .config = config,
            .log_sink = std::make_unique<NullSink>()
        };

        Orchestrator<MockMonitor> orch(std::move(opts));
        orch.monitor().set_cpu(25.0f);
        orch.monitor().set_memory(3ULL * 1024 * 1024 * 1024, 4ULL * 1024 * 1024 * 1024);

        ASSERT_TRUE(orch.start().has_value());

        TaskProfile profile{.compute_cost = Duration{100}, .memory_bytes = 512};
        auto dag = WorkloadGenerator::linear_chain(3, profile);
        auto result = orch.submit_workload(dag);
        ASSERT_TRUE(result.has_value()) << "Policy " << policy_name << " failed";
        EXPECT_EQ(result->completed_tasks, 3u) << "Policy " << policy_name;

        orch.stop();
    }
}
