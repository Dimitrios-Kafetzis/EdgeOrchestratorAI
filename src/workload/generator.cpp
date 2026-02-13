/**
 * @file generator.cpp
 * @brief Synthetic workload generator — all topology implementations.
 * @author Dimitris Kafetzis
 *
 * Generates DAGs that model common AI workload patterns:
 * - Linear chains (sequential pipeline inference)
 * - Fan-out/fan-in (parallel attention heads)
 * - Diamond (multi-layer parallel architectures)
 * - Transformer layers (realistic LLM inference with KV-cache)
 * - Random DAGs (for stress testing and benchmarking)
 */

#include "workload/generator.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace edge_orchestrator {

// ─────────────────────────────────────────────
// Linear Chain: T0 → T1 → T2 → ... → Tn-1
// Models sequential layer-by-layer inference.
// ─────────────────────────────────────────────

WorkloadDAG WorkloadGenerator::linear_chain(size_t num_tasks, TaskProfile base_profile) {
    WorkloadDAG dag;

    TaskId prev_id;
    for (size_t i = 0; i < num_tasks; ++i) {
        Task task{
            .id = std::format("chain_{}", i),
            .name = std::format("Chain Task {}", i),
            .profile = base_profile,
            .dependencies = {},
            .state = TaskState::Pending
        };
        auto id = dag.add_task(std::move(task));
        if (i > 0) {
            dag.add_dependency(prev_id, id);
        }
        prev_id = id;
    }

    return dag;
}

// ─────────────────────────────────────────────
// Fan-out / Fan-in:
//          T_src
//       /   |   \   (backslash)
//     T_0  T_1  T_2  ... T_{width-1}
//       \   |   /
//          T_sink
// Models parallel attention heads or multi-branch architectures.
// ─────────────────────────────────────────────

WorkloadDAG WorkloadGenerator::fan_out_fan_in(size_t width, TaskProfile base_profile) {
    WorkloadDAG dag;

    // Source task (e.g., input projection)
    TaskProfile src_profile = base_profile;
    src_profile.output_bytes = base_profile.input_bytes * width;  // fans out to all branches

    dag.add_task(Task{
        .id = "fan_src",
        .name = "Fan-Out Source",
        .profile = src_profile,
        .dependencies = {},
        .state = TaskState::Pending
    });

    // Parallel branch tasks
    std::vector<TaskId> branch_ids;
    for (size_t i = 0; i < width; ++i) {
        auto branch_id = std::format("fan_branch_{}", i);
        dag.add_task(Task{
            .id = branch_id,
            .name = std::format("Branch {}", i),
            .profile = base_profile,
            .dependencies = {},
            .state = TaskState::Pending
        });
        dag.add_dependency("fan_src", branch_id);
        branch_ids.push_back(branch_id);
    }

    // Sink task (e.g., concatenation + output projection)
    TaskProfile sink_profile = base_profile;
    sink_profile.input_bytes = base_profile.output_bytes * width;

    dag.add_task(Task{
        .id = "fan_sink",
        .name = "Fan-In Sink",
        .profile = sink_profile,
        .dependencies = {},
        .state = TaskState::Pending
    });

    for (const auto& branch_id : branch_ids) {
        dag.add_dependency(branch_id, "fan_sink");
    }

    return dag;
}

// ─────────────────────────────────────────────
// Diamond: Repeated fan-out/fan-in at each depth level.
//
//   Depth 0:        src_0
//              /      |      \    [fan-out]
//   Depth 1: b_1_0  b_1_1  b_1_2
//              \      |      /    [fan-in]
//   Depth 1:      sink_0 = src_1
//              /      |      \    [fan-out]
//   Depth 2: b_2_0  b_2_1  b_2_2
//              \      |      /    [fan-in]
//   Depth 2:        sink_1
//
// Models multi-layer parallel inference pipelines.
// ─────────────────────────────────────────────

WorkloadDAG WorkloadGenerator::diamond(size_t depth, size_t width, TaskProfile base_profile) {
    WorkloadDAG dag;

    TaskId prev_hub;

    for (size_t d = 0; d < depth; ++d) {
        // Hub task at the start of this diamond level
        auto hub_id = std::format("hub_{}", d);
        dag.add_task(Task{
            .id = hub_id,
            .name = std::format("Hub {}", d),
            .profile = base_profile,
            .dependencies = {},
            .state = TaskState::Pending
        });

        if (d > 0) {
            dag.add_dependency(prev_hub, hub_id);
        }

        // Parallel branches
        std::vector<TaskId> branch_ids;
        for (size_t w = 0; w < width; ++w) {
            auto branch_id = std::format("diamond_{}_{}", d, w);
            dag.add_task(Task{
                .id = branch_id,
                .name = std::format("Diamond D{} B{}", d, w),
                .profile = base_profile,
                .dependencies = {},
                .state = TaskState::Pending
            });
            dag.add_dependency(hub_id, branch_id);
            branch_ids.push_back(branch_id);
        }

        // Convergence hub
        auto sink_id = std::format("merge_{}", d);
        dag.add_task(Task{
            .id = sink_id,
            .name = std::format("Merge {}", d),
            .profile = base_profile,
            .dependencies = {},
            .state = TaskState::Pending
        });

        for (const auto& bid : branch_ids) {
            dag.add_dependency(bid, sink_id);
        }

        prev_hub = sink_id;
    }

    return dag;
}

// ─────────────────────────────────────────────
// Transformer Layers:
//
//   For each layer i:
//     layer_{i}_attn  (multi-head attention, memory includes KV-cache)
//         |
//     layer_{i}_ffn   (feed-forward network, 4x hidden dim)
//         |
//     (next layer...)
//
// Realistic model of LLM inference with growing KV-cache memory.
// ─────────────────────────────────────────────

WorkloadDAG WorkloadGenerator::transformer_layers(size_t num_layers,
                                                   uint64_t hidden_dim,
                                                   uint64_t kv_cache_bytes_per_layer) {
    WorkloadDAG dag;

    TaskId prev_id;

    for (size_t i = 0; i < num_layers; ++i) {
        // Attention sub-task
        // Compute cost scales with hidden_dim^2 (self-attention is O(n*d^2))
        // Memory includes KV-cache that grows with layer depth
        auto attn_id = std::format("layer_{}_attn", i);
        uint64_t attn_memory = hidden_dim * hidden_dim * 4  // weight matrices
                             + kv_cache_bytes_per_layer * (i + 1);  // cumulative KV-cache

        dag.add_task(Task{
            .id = attn_id,
            .name = std::format("Layer {} Attention", i),
            .profile = TaskProfile{
                .compute_cost = Duration{static_cast<int64_t>(hidden_dim * 2)},
                .memory_bytes = attn_memory,
                .input_bytes = hidden_dim * sizeof(float),
                .output_bytes = hidden_dim * sizeof(float)
            },
            .dependencies = {},
            .state = TaskState::Pending
        });

        if (i > 0) {
            dag.add_dependency(prev_id, attn_id);
        }

        // FFN sub-task
        // Compute cost is typically 4x hidden_dim (two linear layers with 4*d intermediate)
        // Memory is dominated by the weight matrices
        auto ffn_id = std::format("layer_{}_ffn", i);
        uint64_t ffn_memory = hidden_dim * hidden_dim * 4 * sizeof(float);  // 4*d intermediate

        dag.add_task(Task{
            .id = ffn_id,
            .name = std::format("Layer {} FFN", i),
            .profile = TaskProfile{
                .compute_cost = Duration{static_cast<int64_t>(hidden_dim * 4)},
                .memory_bytes = ffn_memory,
                .input_bytes = hidden_dim * sizeof(float),
                .output_bytes = hidden_dim * sizeof(float)
            },
            .dependencies = {},
            .state = TaskState::Pending
        });

        dag.add_dependency(attn_id, ffn_id);
        prev_id = ffn_id;
    }

    return dag;
}

// ─────────────────────────────────────────────
// Random DAG:
// Generates tasks with random profiles and Erdős–Rényi-style edges.
// Only adds edges from lower-indexed to higher-indexed tasks to
// guarantee acyclicity.
// ─────────────────────────────────────────────

WorkloadDAG WorkloadGenerator::random_dag(size_t num_tasks,
                                           float edge_probability,
                                           TaskProfile min_profile,
                                           TaskProfile max_profile,
                                           std::mt19937& rng) {
    WorkloadDAG dag;

    // Helper to generate a random value in [min, max]
    auto rand_duration = [&](Duration min_d, Duration max_d) -> Duration {
        std::uniform_int_distribution<int64_t> dist(min_d.count(), max_d.count());
        return Duration{dist(rng)};
    };

    auto rand_uint64 = [&](uint64_t min_v, uint64_t max_v) -> uint64_t {
        std::uniform_int_distribution<uint64_t> dist(min_v, max_v);
        return dist(rng);
    };

    // Create tasks with random profiles
    std::vector<TaskId> task_ids;
    task_ids.reserve(num_tasks);

    for (size_t i = 0; i < num_tasks; ++i) {
        auto id = std::format("rand_{}", i);
        TaskProfile profile{
            .compute_cost = rand_duration(min_profile.compute_cost, max_profile.compute_cost),
            .memory_bytes = rand_uint64(min_profile.memory_bytes, max_profile.memory_bytes),
            .input_bytes = rand_uint64(min_profile.input_bytes, max_profile.input_bytes),
            .output_bytes = rand_uint64(min_profile.output_bytes, max_profile.output_bytes)
        };

        dag.add_task(Task{
            .id = id,
            .name = std::format("Random Task {}", i),
            .profile = profile,
            .dependencies = {},
            .state = TaskState::Pending
        });
        task_ids.push_back(id);
    }

    // Add random edges (only from lower to higher index → guaranteed acyclic)
    std::uniform_real_distribution<float> edge_dist(0.0f, 1.0f);
    for (size_t i = 0; i < num_tasks; ++i) {
        for (size_t j = i + 1; j < num_tasks; ++j) {
            if (edge_dist(rng) < edge_probability) {
                dag.add_dependency(task_ids[i], task_ids[j]);
            }
        }
    }

    return dag;
}

}  // namespace edge_orchestrator
