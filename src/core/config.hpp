/**
 * @file config.hpp
 * @brief Daemon configuration with TOML deserialization.
 * @author Dimitris Kafetzis
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "core/result.hpp"

namespace edge_orchestrator {

struct NodeConfig {
    std::string id = "node-01";
    uint16_t port = 5201;
    uint16_t discovery_port = 5200;
};

struct MonitorConfig {
    uint32_t sampling_interval_ms = 500;
    bool mock = false;
};

struct ExecutorConfig {
    uint32_t thread_count = 0;          ///< 0 = hardware_concurrency
    uint64_t memory_pool_mb = 64;
};

struct ThresholdPolicyConfig {
    float cpu_threshold_percent = 75.0f;
    float memory_threshold_percent = 80.0f;
};

struct OptimizerPolicyConfig {
    uint32_t max_iterations = 100;
    float communication_weight = 0.3f;
};

struct SchedulerConfig {
    std::string policy = "greedy";      ///< "greedy", "threshold", "optimizer"
    ThresholdPolicyConfig threshold;
    OptimizerPolicyConfig optimizer;
};

struct NetworkConfig {
    uint32_t heartbeat_interval_ms = 2000;
    uint32_t peer_timeout_ms = 6000;
    uint64_t max_message_size_bytes = 1048576;
};

struct TelemetryConfig {
    std::filesystem::path log_dir = "./logs";
    uint32_t max_file_size_mb = 50;
    uint32_t rotate_count = 5;
    std::string log_level = "info";
};

/**
 * @brief Top-level daemon configuration.
 */
struct Config {
    NodeConfig node;
    MonitorConfig monitor;
    ExecutorConfig executor;
    SchedulerConfig scheduler;
    NetworkConfig network;
    TelemetryConfig telemetry;
};

/**
 * @brief Load configuration from a TOML file.
 */
Result<Config> load_config(const std::filesystem::path& path);

/**
 * @brief Create a default configuration.
 */
Config default_config();

}  // namespace edge_orchestrator
