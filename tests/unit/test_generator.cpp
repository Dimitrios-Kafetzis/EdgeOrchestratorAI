/**
 * @file test_generator.cpp
 * @brief Comprehensive unit tests for WorkloadGenerator.
 * @author Dimitris Kafetzis
 */

#include "workload/generator.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <random>

using namespace edge_orchestrator;

static const TaskProfile BASE_PROFILE{
    .compute_cost = Duration{1000},
    .memory_bytes = 4096,
    .input_bytes = 1024,
    .output_bytes = 1024
};

// ─── Linear Chain ────────────────────────────

TEST(GeneratorTest, LinearChainTaskCount) {
    auto dag = WorkloadGenerator::linear_chain(5, BASE_PROFILE);
    EXPECT_EQ(dag.task_count(), 5u);
}

TEST(GeneratorTest, LinearChainIsValid) {
    auto dag = WorkloadGenerator::linear_chain(10, BASE_PROFILE);
    EXPECT_TRUE(dag.is_valid());
    EXPECT_FALSE(dag.has_cycle());
}

TEST(GeneratorTest, LinearChainDependencies) {
    auto dag = WorkloadGenerator::linear_chain(4, BASE_PROFILE);

    // chain_0 has no dependencies
    EXPECT_TRUE(dag.dependencies("chain_0").empty());

    // Each subsequent task depends on the previous
    for (int i = 1; i < 4; ++i) {
        auto deps = dag.dependencies("chain_" + std::to_string(i));
        ASSERT_EQ(deps.size(), 1u);
        EXPECT_EQ(deps[0], "chain_" + std::to_string(i - 1));
    }
}

TEST(GeneratorTest, LinearChainOnlyFirstReady) {
    auto dag = WorkloadGenerator::linear_chain(5, BASE_PROFILE);
    auto ready = dag.ready_tasks();
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0], "chain_0");
}

TEST(GeneratorTest, LinearChainCriticalPath) {
    TaskProfile p{.compute_cost = Duration{100}, .memory_bytes = 0};
    auto dag = WorkloadGenerator::linear_chain(5, p);
    EXPECT_EQ(dag.critical_path_cost(), Duration{500});
}

TEST(GeneratorTest, LinearChainSingleTask) {
    auto dag = WorkloadGenerator::linear_chain(1, BASE_PROFILE);
    EXPECT_EQ(dag.task_count(), 1u);
    EXPECT_TRUE(dag.is_valid());
    auto ready = dag.ready_tasks();
    ASSERT_EQ(ready.size(), 1u);
}

// ─── Fan-Out / Fan-In ────────────────────────

TEST(GeneratorTest, FanOutFanInTaskCount) {
    auto dag = WorkloadGenerator::fan_out_fan_in(4, BASE_PROFILE);
    // 1 source + 4 branches + 1 sink = 6
    EXPECT_EQ(dag.task_count(), 6u);
}

TEST(GeneratorTest, FanOutFanInIsValid) {
    auto dag = WorkloadGenerator::fan_out_fan_in(3, BASE_PROFILE);
    EXPECT_TRUE(dag.is_valid());
}

TEST(GeneratorTest, FanOutFanInStructure) {
    auto dag = WorkloadGenerator::fan_out_fan_in(3, BASE_PROFILE);

    // Source has no dependencies
    EXPECT_TRUE(dag.dependencies("fan_src").empty());

    // Each branch depends on source
    for (int i = 0; i < 3; ++i) {
        auto deps = dag.dependencies("fan_branch_" + std::to_string(i));
        ASSERT_EQ(deps.size(), 1u);
        EXPECT_EQ(deps[0], "fan_src");
    }

    // Sink depends on all branches
    auto sink_deps = dag.dependencies("fan_sink");
    EXPECT_EQ(sink_deps.size(), 3u);
}

TEST(GeneratorTest, FanOutFanInReadyTasks) {
    auto dag = WorkloadGenerator::fan_out_fan_in(4, BASE_PROFILE);

    // Initially only source is ready
    auto ready = dag.ready_tasks();
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0], "fan_src");

    // After completing source, all branches become ready
    dag.mark_completed("fan_src");
    ready = dag.ready_tasks();
    EXPECT_EQ(ready.size(), 4u);
}

// ─── Diamond ─────────────────────────────────

TEST(GeneratorTest, DiamondTaskCount) {
    // Each level: 1 hub + width branches + 1 merge = 2 + width
    auto dag = WorkloadGenerator::diamond(3, 2, BASE_PROFILE);
    // 3 levels × (1 hub + 2 branches + 1 merge) = 12
    EXPECT_EQ(dag.task_count(), 12u);
}

TEST(GeneratorTest, DiamondIsValid) {
    auto dag = WorkloadGenerator::diamond(4, 3, BASE_PROFILE);
    EXPECT_TRUE(dag.is_valid());
    EXPECT_FALSE(dag.has_cycle());
}

TEST(GeneratorTest, DiamondSingleDepth) {
    auto dag = WorkloadGenerator::diamond(1, 2, BASE_PROFILE);
    // 1 hub + 2 branches + 1 merge = 4
    EXPECT_EQ(dag.task_count(), 4u);
    EXPECT_TRUE(dag.is_valid());
}

// ─── Transformer Layers ─────────────────────

TEST(GeneratorTest, TransformerTaskCount) {
    auto dag = WorkloadGenerator::transformer_layers(6, 768, 512);
    // Each layer: 1 attention + 1 FFN = 2 tasks
    EXPECT_EQ(dag.task_count(), 12u);
}

TEST(GeneratorTest, TransformerIsValid) {
    auto dag = WorkloadGenerator::transformer_layers(12, 768, 1024);
    EXPECT_TRUE(dag.is_valid());
    EXPECT_FALSE(dag.has_cycle());
}

TEST(GeneratorTest, TransformerStructure) {
    auto dag = WorkloadGenerator::transformer_layers(3, 256, 512);

    // Layer 0 attention has no dependencies
    EXPECT_TRUE(dag.dependencies("layer_0_attn").empty());

    // Layer 0 FFN depends on layer 0 attention
    auto ffn0_deps = dag.dependencies("layer_0_ffn");
    ASSERT_EQ(ffn0_deps.size(), 1u);
    EXPECT_EQ(ffn0_deps[0], "layer_0_attn");

    // Layer 1 attention depends on layer 0 FFN
    auto attn1_deps = dag.dependencies("layer_1_attn");
    ASSERT_EQ(attn1_deps.size(), 1u);
    EXPECT_EQ(attn1_deps[0], "layer_0_ffn");
}

TEST(GeneratorTest, TransformerKVCacheGrows) {
    auto dag = WorkloadGenerator::transformer_layers(4, 256, 1024);

    // KV-cache memory should grow with layer depth
    auto attn0 = dag.get_task("layer_0_attn");
    auto attn3 = dag.get_task("layer_3_attn");
    ASSERT_TRUE(attn0.has_value());
    ASSERT_TRUE(attn3.has_value());

    // Layer 3 attention should use more memory than layer 0 due to KV-cache
    EXPECT_GT(attn3->profile.memory_bytes, attn0->profile.memory_bytes);
}

TEST(GeneratorTest, TransformerOnlyFirstReady) {
    auto dag = WorkloadGenerator::transformer_layers(6, 768, 512);
    auto ready = dag.ready_tasks();
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0], "layer_0_attn");
}

// ─── Random DAG ──────────────────────────────

TEST(GeneratorTest, RandomDAGTaskCount) {
    std::mt19937 rng(42);
    TaskProfile min_p{.compute_cost = Duration{100}, .memory_bytes = 1024,
                      .input_bytes = 256, .output_bytes = 256};
    TaskProfile max_p{.compute_cost = Duration{1000}, .memory_bytes = 8192,
                      .input_bytes = 4096, .output_bytes = 4096};

    auto dag = WorkloadGenerator::random_dag(20, 0.3f, min_p, max_p, rng);
    EXPECT_EQ(dag.task_count(), 20u);
}

TEST(GeneratorTest, RandomDAGIsAlwaysAcyclic) {
    std::mt19937 rng(123);
    TaskProfile min_p{.compute_cost = Duration{10}, .memory_bytes = 100};
    TaskProfile max_p{.compute_cost = Duration{500}, .memory_bytes = 5000};

    // Test multiple random seeds — should never produce a cycle
    for (int seed = 0; seed < 10; ++seed) {
        rng.seed(static_cast<unsigned>(seed));
        auto dag = WorkloadGenerator::random_dag(50, 0.5f, min_p, max_p, rng);
        EXPECT_FALSE(dag.has_cycle()) << "Cycle detected with seed " << seed;
        EXPECT_TRUE(dag.is_valid()) << "Invalid DAG with seed " << seed;
    }
}

TEST(GeneratorTest, RandomDAGZeroEdgeProbability) {
    std::mt19937 rng(42);
    TaskProfile p{.compute_cost = Duration{100}, .memory_bytes = 1024};

    auto dag = WorkloadGenerator::random_dag(10, 0.0f, p, p, rng);
    EXPECT_EQ(dag.task_count(), 10u);
    // With 0 edge probability, all tasks are independent
    auto ready = dag.ready_tasks();
    EXPECT_EQ(ready.size(), 10u);
}

TEST(GeneratorTest, RandomDAGFullEdgeProbability) {
    std::mt19937 rng(42);
    TaskProfile p{.compute_cost = Duration{100}, .memory_bytes = 1024};

    auto dag = WorkloadGenerator::random_dag(5, 1.0f, p, p, rng);
    EXPECT_EQ(dag.task_count(), 5u);
    // With probability 1.0, only rand_0 should be ready (all others depend on it)
    auto ready = dag.ready_tasks();
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0], "rand_0");
}
