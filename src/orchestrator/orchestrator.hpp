/**
 * @file orchestrator.hpp
 * @brief Top-level Orchestrator facade — ties all modules together.
 * @author Dimitris Kafetzis
 *
 * Provides a single entry point for:
 *   1. Submitting a workload DAG for scheduling + execution
 *   2. Accepting offloaded tasks from remote peers
 *   3. Monitoring cluster state and resource utilization
 *
 * Template-parameterized on MonitorT for testability (LinuxMonitor or MockMonitor).
 */

#pragma once

#include "core/config.hpp"
#include "core/logger.hpp"
#include "core/result.hpp"
#include "core/types.hpp"
#include "executor/memory_pool.hpp"
#include "executor/task_runner.hpp"
#include "executor/thread_pool.hpp"
#include "network/cluster_view.hpp"
#include "network/peer_discovery.hpp"
#include "network/transport.hpp"
#include "resource_monitor/monitor.hpp"
#include "scheduler/greedy_policy.hpp"
#include "scheduler/optimizer_policy.hpp"
#include "scheduler/scheduler.hpp"
#include "scheduler/threshold_policy.hpp"
#include "telemetry/json_sink.hpp"
#include "telemetry/metrics_collector.hpp"
#include "workload/dag.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace edge_orchestrator {

/**
 * @brief Result of submitting a workload for orchestration.
 */
struct OrchestrationResult {
    SchedulingPlan plan;
    size_t local_tasks = 0;
    size_t offloaded_tasks = 0;
    size_t completed_tasks = 0;
    size_t failed_tasks = 0;
    Duration total_duration{0};
};

/**
 * @brief Serialized task request/response for TCP offloading.
 *
 * Simple binary format:
 *   Request:  [4B task_id_len][task_id][8B compute_cost_us][8B memory_bytes]
 *             [8B input_bytes][8B output_bytes][input_data...]
 *   Response: [1B status (0=ok,1=err)][8B duration_us][8B peak_mem]
 *             [4B error_len][error_msg][output_data...]
 */
struct OffloadCodec {
    static std::vector<uint8_t> encode_request(const std::string& task_id,
                                                const TaskProfile& profile,
                                                const std::vector<uint8_t>& input_data = {});
    static bool decode_request(const std::vector<uint8_t>& data,
                               std::string& task_id,
                               TaskProfile& profile,
                               std::vector<uint8_t>& input_data);

    static std::vector<uint8_t> encode_response(bool success,
                                                 Duration duration,
                                                 uint64_t peak_memory,
                                                 const std::string& error_msg = {},
                                                 const std::vector<uint8_t>& output = {});
    static bool decode_response(const std::vector<uint8_t>& data,
                                bool& success,
                                Duration& duration,
                                uint64_t& peak_memory,
                                std::string& error_msg,
                                std::vector<uint8_t>& output);

    static void put_u64(std::vector<uint8_t>& buf, uint64_t val);
    static void put_u32(std::vector<uint8_t>& buf, uint32_t val);
    static uint64_t get_u64(const uint8_t* p);
    static uint32_t get_u32(const uint8_t* p);
};

/**
 * @brief The top-level Orchestrator that wires all modules together.
 *
 * Template parameters allow injecting MockMonitor for testing.
 */
template <typename MonitorT = LinuxMonitor>
class Orchestrator {
public:
    struct Options {
        Config config;
        std::unique_ptr<ILogSink> log_sink;
        LogLevel log_level = LogLevel::Info;
    };

    explicit Orchestrator(Options opts);
    ~Orchestrator();

    // Non-copyable, non-movable
    Orchestrator(const Orchestrator&) = delete;
    Orchestrator& operator=(const Orchestrator&) = delete;

    // ── Lifecycle ────────────────────────────
    Result<void> start();
    void stop();
    [[nodiscard]] bool is_running() const noexcept { return running_.load(); }

    // ── Workload Submission ──────────────────
    Result<OrchestrationResult> submit_workload(const WorkloadDAG& dag);

    // ── Accessors (for testing) ─────────────
    MonitorT& monitor() { return monitor_; }
    ClusterViewManager& cluster() { return cluster_mgr_; }
    Logger& logger() { return logger_; }
    const Config& config() const { return config_; }
    MetricsCollector& metrics() { return metrics_; }

private:
    std::unique_ptr<ISchedulingPolicy> create_policy() const;
    void handle_offload_request(const std::vector<uint8_t>& request,
                                std::vector<uint8_t>& response);

    Config config_;
    Logger logger_;
    MonitorT monitor_;

    // Executor
    ThreadPool thread_pool_;
    MemoryPool memory_pool_;
    TaskRunner task_runner_;

    // Network
    ClusterViewManager cluster_mgr_;
    PeerDiscovery discovery_;
    TcpTransport offload_server_;

    // Telemetry
    MetricsCollector metrics_;

    std::atomic<bool> running_{false};
};

// ═══════════════════════════════════════════════
// Template Implementation
// ═══════════════════════════════════════════════

template <typename MonitorT>
Orchestrator<MonitorT>::Orchestrator(Options opts)
    : config_(std::move(opts.config))
    , logger_(std::move(opts.log_sink), opts.log_level)
    , monitor_(config_.node.id, config_.monitor.sampling_interval_ms)
    , thread_pool_(config_.executor.thread_count == 0
                   ? std::thread::hardware_concurrency()
                   : config_.executor.thread_count)
    , memory_pool_(static_cast<size_t>(config_.executor.memory_pool_mb) * 1024 * 1024)
    , discovery_(config_.node.id,
                 config_.node.port,
                 config_.network.heartbeat_interval_ms,
                 config_.network.peer_timeout_ms)
    , metrics_(std::make_unique<NullSink>()) {
}

template <typename MonitorT>
Orchestrator<MonitorT>::~Orchestrator() {
    stop();
}

template <typename MonitorT>
Result<void> Orchestrator<MonitorT>::start() {
    if (running_.exchange(true)) {
        return Error{"Already running"};
    }

    logger_.info("Orchestrator starting: node=" + config_.node.id
                 + " port=" + std::to_string(config_.node.port));

    // Start monitor
    monitor_.start();

    // Register discovery callbacks
    discovery_.on_peer_discovered([this](const PeerInfo& peer) {
        cluster_mgr_.update_peer(peer.node_id, peer.resources);
        logger_.info("Peer discovered: " + peer.node_id);
        metrics_.record_peer_event(peer.node_id, "discovered");
    });

    discovery_.on_peer_lost([this](const PeerInfo& peer) {
        cluster_mgr_.remove_peer(peer.node_id);
        logger_.warn("Peer lost: " + peer.node_id);
        metrics_.record_peer_event(peer.node_id, "lost");
    });

    // Start offload server
    auto listen_result = offload_server_.listen(config_.node.port);
    if (listen_result.has_value()) {
        offload_server_.serve([this](const std::vector<uint8_t>& request) -> std::vector<uint8_t> {
            std::vector<uint8_t> response;
            handle_offload_request(request, response);
            return response;
        });
        logger_.info("Offload server listening on port " + std::to_string(config_.node.port));
    } else {
        logger_.warn("Could not start offload server: " + listen_result.error().message);
    }

    // Start discovery
    discovery_.start();

    logger_.info("Orchestrator started successfully");
    return Result<void>{};
}

template <typename MonitorT>
void Orchestrator<MonitorT>::stop() {
    if (!running_.exchange(false)) return;

    logger_.info("Orchestrator shutting down...");
    discovery_.stop();
    offload_server_.stop_serving();
    monitor_.stop();
    logger_.info("Orchestrator stopped");
}

template <typename MonitorT>
Result<OrchestrationResult> Orchestrator<MonitorT>::submit_workload(const WorkloadDAG& dag) {
    auto snap = monitor_.read();
    if (!snap.has_value()) {
        return Error{"Resource monitor not ready"};
    }

    discovery_.update_local_resources(*snap);

    auto cluster_view = cluster_mgr_.snapshot();
    auto policy = create_policy();
    auto plan = policy->schedule(dag, *snap, cluster_view);

    OrchestrationResult result;
    result.plan = plan;

    auto start_time = std::chrono::steady_clock::now();

    std::atomic<size_t> completed{0};
    std::atomic<size_t> failed{0};
    std::stop_source stop_source;

    for (const auto& decision : plan.decisions) {
        auto task = dag.get_task(decision.task_id);
        if (!task) continue;

        metrics_.record_scheduling_decision(decision);

        if (decision.assigned_node == config_.node.id) {
            result.local_tasks++;
            thread_pool_.submit([this, &completed, &failed,
                                  profile = task->profile,
                                  task_id = decision.task_id,
                                  token = stop_source.get_token()]() {
                auto exec_result = task_runner_.execute(task_id, profile,
                                                        memory_pool_, token);
                metrics_.record_task_event(task_id, exec_result.final_state,
                                           exec_result.actual_duration);
                if (exec_result.final_state == TaskState::Completed) {
                    completed.fetch_add(1);
                } else {
                    failed.fetch_add(1);
                }
            });
        } else {
            result.offloaded_tasks++;
            thread_pool_.submit([this, &completed, &failed,
                                  profile = task->profile,
                                  task_id = decision.task_id,
                                  target = decision.assigned_node]() {
                // In a full deployment, we'd resolve target → IP:port via discovery.
                // For now, offloaded tasks execute locally with a log note.
                logger_.debug("Offloading task " + task_id + " → " + target
                              + " (local fallback in test)");
                auto exec_result = task_runner_.execute(task_id, profile,
                    memory_pool_, std::stop_token{});
                metrics_.record_task_event(task_id, exec_result.final_state,
                                           exec_result.actual_duration);
                if (exec_result.final_state == TaskState::Completed) {
                    completed.fetch_add(1);
                } else {
                    failed.fetch_add(1);
                }
            });
        }
    }

    // Wait for all tasks (bounded)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (completed.load() + failed.load() < plan.decisions.size()
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    result.completed_tasks = completed.load();
    result.failed_tasks = failed.load();
    result.total_duration = std::chrono::duration_cast<Duration>(
        std::chrono::steady_clock::now() - start_time);

    metrics_.record_custom("orchestration_complete",
        "{\"local\":" + std::to_string(result.local_tasks)
        + ",\"offloaded\":" + std::to_string(result.offloaded_tasks)
        + ",\"completed\":" + std::to_string(result.completed_tasks)
        + ",\"failed\":" + std::to_string(result.failed_tasks)
        + ",\"duration_us\":" + std::to_string(result.total_duration.count()) + "}");

    return result;
}

template <typename MonitorT>
std::unique_ptr<ISchedulingPolicy> Orchestrator<MonitorT>::create_policy() const {
    if (config_.scheduler.policy == "threshold") {
        return std::make_unique<ThresholdPolicy>(config_.scheduler.threshold);
    }
    if (config_.scheduler.policy == "optimizer") {
        return std::make_unique<OptimizerPolicy>(config_.scheduler.optimizer);
    }
    return std::make_unique<GreedyPolicy>();  // default
}

template <typename MonitorT>
void Orchestrator<MonitorT>::handle_offload_request(
    const std::vector<uint8_t>& request,
    std::vector<uint8_t>& response) {

    std::string task_id;
    TaskProfile profile;
    std::vector<uint8_t> input_data;

    if (!OffloadCodec::decode_request(request, task_id, profile, input_data)) {
        response = OffloadCodec::encode_response(false, Duration{0}, 0,
                                                  "Failed to decode request");
        return;
    }

    logger_.debug("Executing offloaded task: " + task_id);

    auto exec_result = task_runner_.execute(task_id, profile,
                                             memory_pool_, std::stop_token{});

    bool success = (exec_result.final_state == TaskState::Completed);
    std::string err = exec_result.error_message.value_or("");

    response = OffloadCodec::encode_response(success, exec_result.actual_duration,
                                              exec_result.peak_memory_bytes, err);

    metrics_.record_task_event(task_id, exec_result.final_state,
                                exec_result.actual_duration);
}

}  // namespace edge_orchestrator
