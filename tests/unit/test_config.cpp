/**
 * @file test_config.cpp
 * @brief Unit tests for configuration loading.
 * @author Dimitris Kafetzis
 */

#include "core/config.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

using namespace edge_orchestrator;

class ConfigTest : public ::testing::Test {
protected:
    std::filesystem::path temp_dir_;

    void SetUp() override {
        temp_dir_ = std::filesystem::temp_directory_path() / "eo_test_config";
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_dir_);
    }

    std::filesystem::path write_toml(const std::string& content) {
        auto path = temp_dir_ / "test.toml";
        std::ofstream ofs(path);
        ofs << content;
        return path;
    }
};

TEST_F(ConfigTest, DefaultConfig) {
    auto config = default_config();
    EXPECT_EQ(config.node.id, "node-01");
    EXPECT_EQ(config.node.port, 5201);
    EXPECT_EQ(config.scheduler.policy, "greedy");
    EXPECT_EQ(config.executor.thread_count, 0u);
}

TEST_F(ConfigTest, LoadFullConfig) {
    auto path = write_toml(R"(
        [node]
        id = "rpi-42"
        port = 6000
        discovery_port = 6100

        [monitor]
        sampling_interval_ms = 250
        mock = true

        [executor]
        thread_count = 2
        memory_pool_mb = 128

        [scheduler]
        policy = "optimizer"

        [scheduler.threshold]
        cpu_threshold_percent = 60.0
        memory_threshold_percent = 70.0

        [scheduler.optimizer]
        max_iterations = 200
        communication_weight = 0.5

        [network]
        heartbeat_interval_ms = 1000
        peer_timeout_ms = 3000

        [telemetry]
        log_dir = "/tmp/eo_logs"
        log_level = "debug"
    )");

    auto result = load_config(path);
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto& config = *result;
    EXPECT_EQ(config.node.id, "rpi-42");
    EXPECT_EQ(config.node.port, 6000);
    EXPECT_EQ(config.node.discovery_port, 6100);
    EXPECT_TRUE(config.monitor.mock);
    EXPECT_EQ(config.monitor.sampling_interval_ms, 250u);
    EXPECT_EQ(config.executor.thread_count, 2u);
    EXPECT_EQ(config.executor.memory_pool_mb, 128u);
    EXPECT_EQ(config.scheduler.policy, "optimizer");
    EXPECT_FLOAT_EQ(config.scheduler.threshold.cpu_threshold_percent, 60.0f);
    EXPECT_FLOAT_EQ(config.scheduler.optimizer.communication_weight, 0.5f);
    EXPECT_EQ(config.scheduler.optimizer.max_iterations, 200u);
    EXPECT_EQ(config.network.heartbeat_interval_ms, 1000u);
    EXPECT_EQ(config.telemetry.log_level, "debug");
}

TEST_F(ConfigTest, PartialConfig) {
    auto path = write_toml(R"(
        [node]
        id = "partial-node"
    )");

    auto result = load_config(path);
    ASSERT_TRUE(result.has_value());

    // Overridden field
    EXPECT_EQ(result->node.id, "partial-node");
    // Defaults for everything else
    EXPECT_EQ(result->node.port, 5201);
    EXPECT_EQ(result->scheduler.policy, "greedy");
}

TEST_F(ConfigTest, NonexistentFile) {
    auto result = load_config("/nonexistent/path/config.toml");
    EXPECT_FALSE(result.has_value());
}

TEST_F(ConfigTest, MalformedToml) {
    auto path = write_toml("this is [[ not valid toml }}}}");
    auto result = load_config(path);
    EXPECT_FALSE(result.has_value());
}
