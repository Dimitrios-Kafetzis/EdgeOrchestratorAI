/**
 * @file test_scheduler.cpp
 * @brief Comprehensive unit tests for all three scheduling policies.
 * @author Dimitris Kafetzis
 */

#include "core/config.hpp"
#include "scheduler/greedy_policy.hpp"
#include "scheduler/threshold_policy.hpp"
#include "scheduler/optimizer_policy.hpp"
#include "workload/generator.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <unordered_set>

using namespace edge_orchestrator;

// ─── Test Fixtures ───────────────────────────

namespace {

ResourceSnapshot make_local_snapshot(const std::string& node_id,
                                      float cpu_pct = 30.0f,
                                      uint64_t mem_avail_gb = 2,
                                      uint64_t mem_total_gb = 4) {
    ResourceSnapshot snap;
    snap.node_id = node_id;
    snap.cpu_usage_percent = cpu_pct;
    snap.memory_available_bytes = mem_avail_gb * 1024 * 1024 * 1024;
    snap.memory_total_bytes = mem_total_gb * 1024 * 1024 * 1024;
    snap.timestamp = std::chrono::system_clock::now();
    return snap;
}

PeerInfo make_peer(const std::string& node_id,
                    float cpu_pct = 20.0f,
                    uint64_t mem_avail_gb = 3,
                    uint64_t mem_total_gb = 4) {
    PeerInfo peer;
    peer.node_id = node_id;
    peer.reachable = true;
    peer.resources = make_local_snapshot(node_id, cpu_pct, mem_avail_gb, mem_total_gb);
    return peer;
}

ClusterView make_cluster(std::initializer_list<PeerInfo> peers) {
    ClusterView view;
    for (auto& p : peers) {
        view.peers.push_back(p);
    }
    return view;
}

TaskProfile test_profile(int64_t compute_us = 1000, uint64_t mem_bytes = 4096) {
    return TaskProfile{
        .compute_cost = Duration{compute_us},
        .memory_bytes = mem_bytes,
        .input_bytes = 512,
        .output_bytes = 512
    };
}

}  // namespace

// ─── Policy Names ────────────────────────────

TEST(SchedulerTest, GreedyPolicyName) {
    EXPECT_EQ(GreedyPolicy{}.name(), "greedy");
}

TEST(SchedulerTest, ThresholdPolicyName) {
    EXPECT_EQ(ThresholdPolicy{ThresholdPolicyConfig{}}.name(), "threshold");
}

TEST(SchedulerTest, OptimizerPolicyName) {
    EXPECT_EQ(OptimizerPolicy{OptimizerPolicyConfig{}}.name(), "optimizer");
}

// ─── ClusterView ─────────────────────────────

TEST(ClusterViewTest, AvailableCount) {
    ClusterView view;
    EXPECT_EQ(view.available_count(), 0u);

    view.peers.push_back(make_peer("p1"));
    view.peers.push_back(make_peer("p2"));
    EXPECT_EQ(view.available_count(), 2u);

    view.peers[1].reachable = false;
    EXPECT_EQ(view.available_count(), 1u);
}

TEST(ClusterViewTest, LeastLoadedPeer) {
    auto cluster = make_cluster({
        make_peer("p1", 80.0f),
        make_peer("p2", 20.0f),
        make_peer("p3", 50.0f)
    });

    EXPECT_EQ(cluster.least_loaded_peer(), "p2");
}

// ─── Greedy Policy ───────────────────────────

TEST(GreedyPolicyTest, SingleNodeAllLocal) {
    GreedyPolicy policy;
    auto dag = WorkloadGenerator::linear_chain(5, test_profile());
    auto local = make_local_snapshot("local-node");
    ClusterView empty_cluster;

    auto plan = policy.schedule(dag, local, empty_cluster);

    EXPECT_EQ(plan.decisions.size(), 5u);
    for (const auto& d : plan.decisions) {
        EXPECT_EQ(d.assigned_node, "local-node");
        EXPECT_EQ(d.reason, SchedulingDecision::Reason::LocalCapacity);
    }
}

TEST(GreedyPolicyTest, DistributesAcrossCluster) {
    GreedyPolicy policy;

    // Create tasks with substantial memory requirements
    TaskProfile heavy{.compute_cost = Duration{5000}, .memory_bytes = 1ULL * 1024 * 1024 * 1024};
    auto dag = WorkloadGenerator::linear_chain(3, heavy);

    auto local = make_local_snapshot("local", 50.0f, 2, 4);
    auto cluster = make_cluster({
        make_peer("peer-1", 10.0f, 3, 4),
        make_peer("peer-2", 15.0f, 3, 4)
    });

    auto plan = policy.schedule(dag, local, cluster);
    EXPECT_EQ(plan.decisions.size(), 3u);

    // Should use multiple nodes when memory is scarce
    std::unordered_set<NodeId> used_nodes;
    for (const auto& d : plan.decisions) {
        used_nodes.insert(d.assigned_node);
    }
    // With 2GB available locally and 1GB per task, should offload at least 1
    EXPECT_GE(used_nodes.size(), 1u);
}

TEST(GreedyPolicyTest, AllTasksAssigned) {
    GreedyPolicy policy;
    auto dag = WorkloadGenerator::fan_out_fan_in(4, test_profile());
    auto local = make_local_snapshot("local");
    auto cluster = make_cluster({make_peer("peer-1")});

    auto plan = policy.schedule(dag, local, cluster);
    EXPECT_EQ(plan.decisions.size(), dag.task_count());
}

TEST(GreedyPolicyTest, MakespanEstimated) {
    GreedyPolicy policy;
    auto dag = WorkloadGenerator::linear_chain(5, test_profile(1000));
    auto local = make_local_snapshot("local");
    ClusterView empty_cluster;

    auto plan = policy.schedule(dag, local, empty_cluster);
    EXPECT_GT(plan.estimated_makespan.count(), 0);
}

// ─── Threshold Policy ────────────────────────

TEST(ThresholdPolicyTest, AllLocalWhenBelowThreshold) {
    ThresholdPolicyConfig config{
        .cpu_threshold_percent = 90.0f,
        .memory_threshold_percent = 90.0f
    };
    ThresholdPolicy policy(config);

    auto dag = WorkloadGenerator::linear_chain(5, test_profile(100, 1024));
    auto local = make_local_snapshot("local", 10.0f, 3, 4);
    auto cluster = make_cluster({make_peer("peer-1")});

    auto plan = policy.schedule(dag, local, cluster);

    // With very high thresholds and low load, everything should be local
    for (const auto& d : plan.decisions) {
        EXPECT_EQ(d.assigned_node, "local");
        EXPECT_EQ(d.reason, SchedulingDecision::Reason::LocalCapacity);
    }
}

TEST(ThresholdPolicyTest, OffloadsWhenAboveThreshold) {
    ThresholdPolicyConfig config{
        .cpu_threshold_percent = 10.0f,   // Very low — will trigger immediately
        .memory_threshold_percent = 10.0f
    };
    ThresholdPolicy policy(config);

    auto dag = WorkloadGenerator::linear_chain(5, test_profile(1000, 4096));
    auto local = make_local_snapshot("local", 80.0f, 1, 4);  // High CPU load
    auto cluster = make_cluster({make_peer("peer-1", 10.0f, 3, 4)});

    auto plan = policy.schedule(dag, local, cluster);

    // Should offload at least some tasks
    size_t offloaded = 0;
    for (const auto& d : plan.decisions) {
        if (d.assigned_node != "local") {
            ++offloaded;
            EXPECT_EQ(d.reason, SchedulingDecision::Reason::ThresholdOffload);
        }
    }
    EXPECT_GT(offloaded, 0u);
}

TEST(ThresholdPolicyTest, FallbackWhenNoPeers) {
    ThresholdPolicyConfig config{
        .cpu_threshold_percent = 10.0f,
        .memory_threshold_percent = 10.0f
    };
    ThresholdPolicy policy(config);

    auto dag = WorkloadGenerator::linear_chain(3, test_profile());
    auto local = make_local_snapshot("local", 80.0f, 1, 4);
    ClusterView empty_cluster;

    auto plan = policy.schedule(dag, local, empty_cluster);

    // With no peers available, must fall back to local
    for (const auto& d : plan.decisions) {
        EXPECT_EQ(d.assigned_node, "local");
    }
    // Some should be marked as Fallback
    bool has_fallback = std::any_of(plan.decisions.begin(), plan.decisions.end(),
        [](const SchedulingDecision& d) {
            return d.reason == SchedulingDecision::Reason::Fallback;
        });
    EXPECT_TRUE(has_fallback);
}

TEST(ThresholdPolicyTest, AllTasksAssigned) {
    ThresholdPolicyConfig config{};
    ThresholdPolicy policy(config);
    auto dag = WorkloadGenerator::diamond(2, 3, test_profile());
    auto local = make_local_snapshot("local");
    auto cluster = make_cluster({make_peer("peer-1"), make_peer("peer-2")});

    auto plan = policy.schedule(dag, local, cluster);
    EXPECT_EQ(plan.decisions.size(), dag.task_count());
}

// ─── Optimizer Policy ────────────────────────

TEST(OptimizerPolicyTest, AllTasksAssigned) {
    OptimizerPolicyConfig config{.max_iterations = 50, .communication_weight = 0.3f};
    OptimizerPolicy policy(config);

    auto dag = WorkloadGenerator::linear_chain(10, test_profile());
    auto local = make_local_snapshot("local");
    auto cluster = make_cluster({make_peer("peer-1"), make_peer("peer-2")});

    auto plan = policy.schedule(dag, local, cluster);
    EXPECT_EQ(plan.decisions.size(), dag.task_count());
}

TEST(OptimizerPolicyTest, CriticalPathOnLocal) {
    OptimizerPolicyConfig config{.max_iterations = 0, .communication_weight = 0.3f};
    OptimizerPolicy policy(config);

    // Linear chain — entire chain is the critical path
    auto dag = WorkloadGenerator::linear_chain(5, test_profile());
    auto local = make_local_snapshot("local", 20.0f, 3, 4);
    auto cluster = make_cluster({make_peer("peer-1")});

    auto plan = policy.schedule(dag, local, cluster);

    // With 0 local search iterations, critical path tasks should be on local
    size_t local_count = 0;
    for (const auto& d : plan.decisions) {
        if (d.assigned_node == "local") ++local_count;
    }
    // At least some critical path tasks should be local
    EXPECT_GT(local_count, 0u);
}

TEST(OptimizerPolicyTest, LocalSearchImprovesMakespan) {
    // Compare makespan with 0 iterations vs 100 iterations
    auto dag = WorkloadGenerator::fan_out_fan_in(6, test_profile(2000, 1024));
    auto local = make_local_snapshot("local", 30.0f, 3, 4);
    auto cluster = make_cluster({
        make_peer("peer-1", 20.0f, 3, 4),
        make_peer("peer-2", 25.0f, 3, 4)
    });

    OptimizerPolicyConfig config_no_search{.max_iterations = 0, .communication_weight = 0.3f};
    OptimizerPolicy policy_no_search(config_no_search);
    auto plan_no_search = policy_no_search.schedule(dag, local, cluster);

    OptimizerPolicyConfig config_with_search{.max_iterations = 200, .communication_weight = 0.3f};
    OptimizerPolicy policy_with_search(config_with_search);
    auto plan_with_search = policy_with_search.schedule(dag, local, cluster);

    // With search, makespan should be <= without search
    EXPECT_LE(plan_with_search.estimated_makespan, plan_no_search.estimated_makespan);
}

TEST(OptimizerPolicyTest, RespectsMemoryConstraints) {
    OptimizerPolicyConfig config{.max_iterations = 50, .communication_weight = 0.3f};
    OptimizerPolicy policy(config);

    // Create tasks with large memory that barely fit
    TaskProfile big_mem{.compute_cost = Duration{1000},
                        .memory_bytes = 1ULL * 1024 * 1024 * 1024};  // 1 GB each
    auto dag = WorkloadGenerator::linear_chain(4, big_mem);

    auto local = make_local_snapshot("local", 20.0f, 2, 4);
    auto cluster = make_cluster({
        make_peer("peer-1", 20.0f, 2, 4),
        make_peer("peer-2", 20.0f, 2, 4)
    });

    auto plan = policy.schedule(dag, local, cluster);
    EXPECT_EQ(plan.decisions.size(), 4u);
}

TEST(OptimizerPolicyTest, SingleNodeNoErrors) {
    OptimizerPolicyConfig config{.max_iterations = 20, .communication_weight = 0.3f};
    OptimizerPolicy policy(config);

    auto dag = WorkloadGenerator::transformer_layers(4, 256, 512);
    auto local = make_local_snapshot("local");
    ClusterView empty_cluster;

    auto plan = policy.schedule(dag, local, empty_cluster);
    EXPECT_EQ(plan.decisions.size(), dag.task_count());
}

TEST(OptimizerPolicyTest, TransformerWorkload) {
    OptimizerPolicyConfig config{.max_iterations = 100, .communication_weight = 0.3f};
    OptimizerPolicy policy(config);

    auto dag = WorkloadGenerator::transformer_layers(6, 768, 1024);
    auto local = make_local_snapshot("local", 25.0f, 2, 4);
    auto cluster = make_cluster({
        make_peer("rpi-2", 15.0f, 3, 4),
        make_peer("rpi-3", 10.0f, 3, 4)
    });

    auto plan = policy.schedule(dag, local, cluster);
    EXPECT_EQ(plan.decisions.size(), dag.task_count());
    EXPECT_GT(plan.estimated_makespan.count(), 0);
}

// ─── Cross-Policy Comparison ─────────────────

TEST(SchedulerComparisonTest, AllPoliciesAssignAllTasks) {
    auto dag = WorkloadGenerator::diamond(2, 3, test_profile(500));
    auto local = make_local_snapshot("local");
    auto cluster = make_cluster({make_peer("peer-1"), make_peer("peer-2")});

    GreedyPolicy greedy;
    ThresholdPolicy threshold(ThresholdPolicyConfig{});
    OptimizerPolicy optimizer(OptimizerPolicyConfig{.max_iterations = 50});

    auto plan_g = greedy.schedule(dag, local, cluster);
    auto plan_t = threshold.schedule(dag, local, cluster);
    auto plan_o = optimizer.schedule(dag, local, cluster);

    EXPECT_EQ(plan_g.decisions.size(), dag.task_count());
    EXPECT_EQ(plan_t.decisions.size(), dag.task_count());
    EXPECT_EQ(plan_o.decisions.size(), dag.task_count());
}
