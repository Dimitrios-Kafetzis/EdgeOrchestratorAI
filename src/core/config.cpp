/**
 * @file config.cpp
 * @brief Configuration loading from TOML files using toml++.
 * @author Dimitris Kafetzis
 */

#include "core/config.hpp"

#include <toml++/toml.hpp>

namespace edge_orchestrator {

Result<Config> load_config(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return Error{"Configuration file not found: " + path.string()};
    }

    try {
        auto tbl = toml::parse_file(path.string());
        Config config;

        // [node]
        if (auto node = tbl["node"]; node.is_table()) {
            config.node.id = node["id"].value_or(std::string{"node-01"});
            config.node.port = static_cast<uint16_t>(
                node["port"].value_or(int64_t{5201}));
            config.node.discovery_port = static_cast<uint16_t>(
                node["discovery_port"].value_or(int64_t{5200}));
        }

        // [monitor]
        if (auto monitor = tbl["monitor"]; monitor.is_table()) {
            config.monitor.sampling_interval_ms = static_cast<uint32_t>(
                monitor["sampling_interval_ms"].value_or(int64_t{500}));
            config.monitor.mock = monitor["mock"].value_or(false);
        }

        // [executor]
        if (auto executor = tbl["executor"]; executor.is_table()) {
            config.executor.thread_count = static_cast<uint32_t>(
                executor["thread_count"].value_or(int64_t{0}));
            config.executor.memory_pool_mb = static_cast<uint64_t>(
                executor["memory_pool_mb"].value_or(int64_t{64}));
        }

        // [scheduler]
        if (auto scheduler = tbl["scheduler"]; scheduler.is_table()) {
            config.scheduler.policy = scheduler["policy"].value_or(std::string{"greedy"});

            // [scheduler.threshold]
            if (auto threshold = scheduler["threshold"]; threshold.is_table()) {
                config.scheduler.threshold.cpu_threshold_percent =
                    static_cast<float>(threshold["cpu_threshold_percent"].value_or(75.0));
                config.scheduler.threshold.memory_threshold_percent =
                    static_cast<float>(threshold["memory_threshold_percent"].value_or(80.0));
            }

            // [scheduler.optimizer]
            if (auto optimizer = scheduler["optimizer"]; optimizer.is_table()) {
                config.scheduler.optimizer.max_iterations = static_cast<uint32_t>(
                    optimizer["max_iterations"].value_or(int64_t{100}));
                config.scheduler.optimizer.communication_weight =
                    static_cast<float>(optimizer["communication_weight"].value_or(0.3));
            }
        }

        // [network]
        if (auto network = tbl["network"]; network.is_table()) {
            config.network.heartbeat_interval_ms = static_cast<uint32_t>(
                network["heartbeat_interval_ms"].value_or(int64_t{2000}));
            config.network.peer_timeout_ms = static_cast<uint32_t>(
                network["peer_timeout_ms"].value_or(int64_t{6000}));
            config.network.max_message_size_bytes = static_cast<uint64_t>(
                network["max_message_size_bytes"].value_or(int64_t{1048576}));
        }

        // [telemetry]
        if (auto telemetry = tbl["telemetry"]; telemetry.is_table()) {
            config.telemetry.log_dir = telemetry["log_dir"].value_or(std::string{"./logs"});
            config.telemetry.max_file_size_mb = static_cast<uint32_t>(
                telemetry["max_file_size_mb"].value_or(int64_t{50}));
            config.telemetry.rotate_count = static_cast<uint32_t>(
                telemetry["rotate_count"].value_or(int64_t{5}));
            config.telemetry.log_level = telemetry["log_level"].value_or(std::string{"info"});
        }

        return config;

    } catch (const toml::parse_error& err) {
        return Error{std::string{"TOML parse error: "} + std::string{err.description()}};
    }
}

Config default_config() {
    return Config{};
}

}  // namespace edge_orchestrator
