/**
 * @file generator.hpp
 * @brief Synthetic workload generators for testing and benchmarking.
 * @author Dimitris Kafetzis
 */

#pragma once

#include "workload/dag.hpp"

#include <random>

namespace edge_orchestrator {

/**
 * @brief Factory for synthetic workload DAGs with various topologies.
 */
class WorkloadGenerator {
public:
    /// Linear chain: T1 → T2 → ... → Tn (sequential pipeline)
    static WorkloadDAG linear_chain(size_t num_tasks, TaskProfile base_profile);

    /// Fan-out / Fan-in: T1 → {T2a, T2b, ...} → Tn (parallel stage)
    static WorkloadDAG fan_out_fan_in(size_t width, TaskProfile base_profile);

    /// Diamond: repeated fan-out/fan-in at each depth level
    static WorkloadDAG diamond(size_t depth, size_t width, TaskProfile base_profile);

    /// Transformer layers: attention + FFN per layer with KV-cache scaling
    static WorkloadDAG transformer_layers(size_t num_layers,
                                          uint64_t hidden_dim,
                                          uint64_t kv_cache_bytes_per_layer);

    /// Random DAG with configurable edge probability
    static WorkloadDAG random_dag(size_t num_tasks,
                                  float edge_probability,
                                  TaskProfile min_profile,
                                  TaskProfile max_profile,
                                  std::mt19937& rng);
};

}  // namespace edge_orchestrator
