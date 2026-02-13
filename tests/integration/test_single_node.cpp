/**
 * @file test_single_node.cpp
 * @brief Integration tests exercising the full orchestration pipeline.
 * @author Dimitris Kafetzis
 */

#include "orchestrator/orchestrator.hpp"
#include "core/config.hpp"
#include "core/logger.hpp"
#include "core/types.hpp"
#include "executor/memory_pool.hpp"
#include "executor/task_runner.hpp"
#include "executor/thread_pool.hpp"
#include "network/cluster_view.hpp"
#include "network/peer_discovery.hpp"
#include "network/transport.hpp"
#include "resource_monitor/monitor.hpp"
#include "scheduler/greedy_policy.hpp"
#include "scheduler/threshold_policy.hpp"
#include "scheduler/optimizer_policy.hpp"
#include "telemetry/json_sink.hpp"
#include "telemetry/metrics_collector.hpp"
#include "workload/generator.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace edge_orchestrator;

// ═══════════════════════════════════════════════
// Single-Node Pipeline Tests
// ═══════════════════════════════════════════════

TEST(SingleNodeIntegration, ExecuteSyntheticTask) {
    MemoryPool pool(1024 * 1024);
    TaskRunner runner;
    std::stop_source stop_source;

    TaskProfile profile{
        .compute_cost = Duration{1000},
        .memory_bytes = 4096,
        .input_bytes = 0,
        .output_bytes = 0
    };

    auto result = runner.execute("test_task_1", profile, pool, stop_source.get_token());
    EXPECT_EQ(result.final_state, TaskState::Completed);
    EXPECT_EQ(result.task_id, "test_task_1");
    EXPECT_GT(result.actual_duration.count(), 0);
    EXPECT_EQ(result.peak_memory_bytes, 4096u);
    EXPECT_FALSE(result.error_message.has_value());
}

TEST(SingleNodeIntegration, GenerateAndCountTasks) {
    TaskProfile profile{.compute_cost = Duration{500}, .memory_bytes = 2048};
    auto dag = WorkloadGenerator::linear_chain(10, profile);
    EXPECT_EQ(dag.task_count(), 10u);
    EXPECT_EQ(dag.total_compute_cost(), Duration{5000});
}

TEST(SingleNodeIntegration, MockMonitorReads) {
    MockMonitor monitor("test-node");
    monitor.start();
    auto result = monitor.read();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->node_id, "test-node");
    EXPECT_FLOAT_EQ(result->cpu_usage_percent, 25.0f);
    monitor.stop();
}

// ═══════════════════════════════════════════════
// Full Pipeline: Monitor → Scheduler → Executor
// ═══════════════════════════════════════════════

TEST(PipelineIntegration, MonitorToSchedulerToExecutor) {
    MockMonitor monitor("pipeline-node");
    monitor.set_cpu(30.0f);
    monitor.set_memory(3ULL * 1024 * 1024 * 1024, 4ULL * 1024 * 1024 * 1024);
    monitor.start();

    auto snap = monitor.read();
    ASSERT_TRUE(snap.has_value());

    TaskProfile profile{.compute_cost = Duration{500}, .memory_bytes = 1024};
    auto dag = WorkloadGenerator::linear_chain(5, profile);

    GreedyPolicy scheduler;
    ClusterView empty_cluster;
    auto plan = scheduler.schedule(dag, *snap, empty_cluster);

    EXPECT_EQ(plan.decisions.size(), 5u);
    for (const auto& d : plan.decisions) {
        EXPECT_EQ(d.assigned_node, "pipeline-node");
    }

    ThreadPool pool(2);
    MemoryPool mem_pool(4 * 1024 * 1024);
    TaskRunner runner;
    std::stop_source stop_source;

    std::atomic<size_t> completed{0};
    for (const auto& decision : plan.decisions) {
        auto task = dag.get_task(decision.task_id);
        ASSERT_TRUE(task.has_value());
        pool.submit([&, profile = task->profile, task_id = decision.task_id]() {
            auto result = runner.execute(task_id, profile, mem_pool,
                                          stop_source.get_token());
            if (result.final_state == TaskState::Completed) {
                completed.fetch_add(1);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(completed.load(), 5u);
    monitor.stop();
}

TEST(PipelineIntegration, TransformerWorkloadEndToEnd) {
    MockMonitor monitor("rpi-4b");
    monitor.set_cpu(20.0f);
    monitor.set_memory(3ULL * 1024 * 1024 * 1024, 4ULL * 1024 * 1024 * 1024);
    monitor.start();

    auto snap = monitor.read();
    ASSERT_TRUE(snap.has_value());

    auto dag = WorkloadGenerator::transformer_layers(4, 256, 512);
    EXPECT_EQ(dag.task_count(), 8u);

    OptimizerPolicyConfig opt_config{.max_iterations = 50, .communication_weight = 0.3f};
    OptimizerPolicy optimizer(opt_config);
    ClusterView empty_cluster;
    auto plan = optimizer.schedule(dag, *snap, empty_cluster);

    EXPECT_EQ(plan.decisions.size(), 8u);
    // Makespan may be 0 for single-node (no local search improvement possible)

    ThreadPool pool(4);
    MemoryPool mem_pool(16 * 1024 * 1024);
    TaskRunner runner;
    std::stop_source stop_source;

    std::atomic<size_t> completed{0};
    for (const auto& decision : plan.decisions) {
        auto task = dag.get_task(decision.task_id);
        if (!task) continue;
        pool.submit([&, p = task->profile, id = decision.task_id]() {
            runner.execute(id, p, mem_pool, stop_source.get_token());
            completed.fetch_add(1);
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(completed.load(), 8u);
    monitor.stop();
}

TEST(PipelineIntegration, ThresholdPolicyWithPeers) {
    MockMonitor monitor("overloaded-node");
    monitor.set_cpu(85.0f);
    monitor.set_memory(512 * 1024 * 1024, 4ULL * 1024 * 1024 * 1024);
    monitor.start();

    auto snap = monitor.read();
    ASSERT_TRUE(snap.has_value());

    ClusterViewManager cluster_mgr;
    ResourceSnapshot peer_snap;
    peer_snap.node_id = "idle-peer";
    peer_snap.cpu_usage_percent = 10.0f;
    peer_snap.memory_available_bytes = 3ULL * 1024 * 1024 * 1024;
    peer_snap.memory_total_bytes = 4ULL * 1024 * 1024 * 1024;
    cluster_mgr.update_peer("idle-peer", peer_snap);

    auto cluster_view = cluster_mgr.snapshot();
    EXPECT_EQ(cluster_view.available_count(), 1u);

    TaskProfile profile{.compute_cost = Duration{1000}, .memory_bytes = 4096};
    auto dag = WorkloadGenerator::fan_out_fan_in(4, profile);

    ThresholdPolicyConfig threshold_config{
        .cpu_threshold_percent = 50.0f,
        .memory_threshold_percent = 50.0f
    };
    ThresholdPolicy policy(threshold_config);
    auto plan = policy.schedule(dag, *snap, cluster_view);

    EXPECT_EQ(plan.decisions.size(), dag.task_count());

    size_t offloaded = 0;
    for (const auto& d : plan.decisions) {
        if (d.assigned_node != "overloaded-node") ++offloaded;
    }
    EXPECT_GT(offloaded, 0u) << "Should offload tasks when above threshold";
    monitor.stop();
}

// ═══════════════════════════════════════════════
// Telemetry Integration
// ═══════════════════════════════════════════════

TEST(TelemetryIntegration, MetricsCollectionDuringExecution) {
    auto sink = std::make_unique<NullSink>();
    MetricsCollector metrics(std::move(sink));

    ResourceSnapshot snap;
    snap.node_id = "test";
    snap.cpu_usage_percent = 45.0f;
    snap.memory_total_bytes = 4ULL * 1024 * 1024 * 1024;
    snap.memory_available_bytes = 2ULL * 1024 * 1024 * 1024;
    metrics.record_resource_snapshot(snap);

    SchedulingDecision decision{
        .task_id = "task-0",
        .assigned_node = "node-1",
        .reason = SchedulingDecision::Reason::LeastLoaded
    };
    metrics.record_scheduling_decision(decision);

    metrics.record_task_event("task-0", TaskState::Completed, Duration{1500});
    metrics.record_peer_event("node-1", "discovered");
    metrics.record_custom("test_event", "{\"key\":\"value\"}");

    SUCCEED();
}

// ═══════════════════════════════════════════════
// Two-Node Communication Test
// ═══════════════════════════════════════════════

TEST(TwoNodeIntegration, TcpOffloadRoundTrip) {
    TcpTransport server;
    uint16_t port = 19950;
    auto listen_result = server.listen(port);
    if (!listen_result.has_value()) {
        GTEST_SKIP() << "Could not bind to port " << port;
    }

    std::atomic<bool> request_received{false};
    server.serve([&](const std::vector<uint8_t>& request) -> std::vector<uint8_t> {
        request_received = true;
        std::vector<uint8_t> response;
        response.push_back(0x01);  // success status
        response.insert(response.end(), request.begin(), request.end());
        return response;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TcpTransport client;
    auto connect_result = client.connect("127.0.0.1", port, 2000);
    ASSERT_TRUE(connect_result.has_value());

    std::vector<uint8_t> request = {'T', '0', '0', '1', 0x00, 0x03, 0xE8, 0x00};
    auto send_result = client.send(request);
    ASSERT_TRUE(send_result.has_value());

    auto recv_result = client.receive(5000);
    ASSERT_TRUE(recv_result.has_value());

    EXPECT_TRUE(request_received.load());
    auto& response = *recv_result;
    ASSERT_GT(response.size(), 0u);
    EXPECT_EQ(response[0], 0x01);
    EXPECT_EQ(response.size(), request.size() + 1);

    client.disconnect();
    server.stop_serving();
}

TEST(TwoNodeIntegration, DiscoveryAndClusterViewUpdate) {
    ClusterViewManager cluster_mgr;

    PeerDiscovery disc_a("node-alpha", 19951, 200, 2000);
    PeerDiscovery disc_b("node-beta", 19951, 200, 2000);

    ResourceSnapshot snap_a;
    snap_a.cpu_usage_percent = 30.0f;
    snap_a.memory_available_bytes = 2ULL * 1024 * 1024 * 1024;
    snap_a.memory_total_bytes = 4ULL * 1024 * 1024 * 1024;
    disc_a.update_local_resources(snap_a);

    ResourceSnapshot snap_b;
    snap_b.cpu_usage_percent = 60.0f;
    snap_b.memory_available_bytes = 1ULL * 1024 * 1024 * 1024;
    snap_b.memory_total_bytes = 4ULL * 1024 * 1024 * 1024;
    disc_b.update_local_resources(snap_b);

    disc_a.on_peer_discovered([&](const PeerInfo& peer) {
        cluster_mgr.update_peer(peer.node_id, peer.resources);
    });
    disc_b.on_peer_discovered([&](const PeerInfo& peer) {
        cluster_mgr.update_peer(peer.node_id, peer.resources);
    });

    disc_a.start();
    disc_b.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    disc_a.stop();
    disc_b.stop();

    EXPECT_EQ(cluster_mgr.cluster_size(), 2u);
    EXPECT_TRUE(cluster_mgr.has_peer("node-alpha"));
    EXPECT_TRUE(cluster_mgr.has_peer("node-beta"));

    auto alpha_res = cluster_mgr.peer_resources("node-alpha");
    auto beta_res = cluster_mgr.peer_resources("node-beta");
    ASSERT_TRUE(alpha_res.has_value());
    ASSERT_TRUE(beta_res.has_value());
    EXPECT_FLOAT_EQ(alpha_res->cpu_usage_percent, 30.0f);
    EXPECT_FLOAT_EQ(beta_res->cpu_usage_percent, 60.0f);

    // Schedule using the discovered cluster
    auto view = cluster_mgr.snapshot();
    EXPECT_EQ(view.available_count(), 2u);

    TaskProfile profile{.compute_cost = Duration{1000}, .memory_bytes = 4096};
    auto dag = WorkloadGenerator::linear_chain(5, profile);

    GreedyPolicy scheduler;
    ResourceSnapshot local_snap;
    local_snap.node_id = "node-alpha";
    local_snap.cpu_usage_percent = 30.0f;
    local_snap.memory_available_bytes = 2ULL * 1024 * 1024 * 1024;
    local_snap.memory_total_bytes = 4ULL * 1024 * 1024 * 1024;

    auto plan = scheduler.schedule(dag, local_snap, view);
    EXPECT_EQ(plan.decisions.size(), 5u);
}

// ═══════════════════════════════════════════════
// Full E2E: Monitor → Discovery → Schedule → Execute
// ═══════════════════════════════════════════════

TEST(EndToEndIntegration, FullOrchestrationPipeline) {
    // 1. Configuration
    auto config = default_config();
    config.node.id = "e2e-node";

    // 2. Logger
    auto log_sink = std::make_unique<NullSink>();
    Logger logger(std::move(log_sink), LogLevel::Debug);

    // 3. Resource Monitor
    LinuxMonitor monitor(config.node.id, 100);
    monitor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto snap = monitor.read();
    ASSERT_TRUE(snap.has_value());
    EXPECT_GT(snap->memory_total_bytes, 0u);

    // 4. Cluster View (single-node, no peers)
    ClusterViewManager cluster_mgr;
    auto cluster_view = cluster_mgr.snapshot();
    EXPECT_EQ(cluster_view.available_count(), 0u);

    // 5. Generate workload
    auto dag = WorkloadGenerator::diamond(2, 3,
        TaskProfile{.compute_cost = Duration{200}, .memory_bytes = 1024});

    // 6. Schedule
    GreedyPolicy scheduler;
    auto plan = scheduler.schedule(dag, *snap, cluster_view);
    EXPECT_EQ(plan.decisions.size(), dag.task_count());

    // 7. Execute
    ThreadPool pool(2);
    MemoryPool mem_pool(8 * 1024 * 1024);
    TaskRunner runner;
    std::stop_source stop_source;

    std::atomic<size_t> completed{0};
    for (const auto& d : plan.decisions) {
        auto task = dag.get_task(d.task_id);
        if (!task) continue;
        pool.submit([&, p = task->profile, id = d.task_id]() {
            runner.execute(id, p, mem_pool, stop_source.get_token());
            completed.fetch_add(1);
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(completed.load(), dag.task_count());

    // 8. Telemetry
    auto metrics_sink = std::make_unique<NullSink>();
    MetricsCollector metrics(std::move(metrics_sink));
    metrics.record_custom("e2e_complete",
        "{\"tasks\":" + std::to_string(completed.load())
        + ",\"makespan_us\":" + std::to_string(plan.estimated_makespan.count()) + "}");

    monitor.stop();
    logger.info("End-to-end pipeline test complete");
}

TEST(EndToEndIntegration, DemoModeEquivalent) {
    // Replicates the --demo path programmatically
    auto config = default_config();
    config.node.id = "demo-node";

    auto dag = WorkloadGenerator::transformer_layers(4, 256, 512);
    EXPECT_EQ(dag.task_count(), 8u);
    EXPECT_GT(dag.critical_path_cost().count(), 0);
    EXPECT_GT(dag.peak_memory_estimate(), 0u);

    MockMonitor monitor(config.node.id);
    monitor.set_cpu(25.0f);
    monitor.set_memory(3ULL * 1024 * 1024 * 1024, 4ULL * 1024 * 1024 * 1024);
    monitor.start();
    auto snap = monitor.read();
    ASSERT_TRUE(snap.has_value());

    // Compare all three policies
    ClusterView empty_cluster;
    GreedyPolicy greedy;
    ThresholdPolicy threshold(config.scheduler.threshold);
    OptimizerPolicy optimizer(config.scheduler.optimizer);

    auto plan_g = greedy.schedule(dag, *snap, empty_cluster);
    auto plan_t = threshold.schedule(dag, *snap, empty_cluster);
    auto plan_o = optimizer.schedule(dag, *snap, empty_cluster);

    // All must assign all tasks
    EXPECT_EQ(plan_g.decisions.size(), 8u);
    EXPECT_EQ(plan_t.decisions.size(), 8u);
    EXPECT_EQ(plan_o.decisions.size(), 8u);

    // All local when no peers
    for (const auto& d : plan_g.decisions) EXPECT_EQ(d.assigned_node, "demo-node");
    for (const auto& d : plan_t.decisions) EXPECT_EQ(d.assigned_node, "demo-node");

    monitor.stop();
}
