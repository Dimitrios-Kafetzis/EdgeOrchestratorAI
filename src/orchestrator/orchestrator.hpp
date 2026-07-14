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

#include "core/concepts.hpp"
#include "core/config.hpp"
#include "core/logger.hpp"
#include "core/result.hpp"
#include "core/types.hpp"
#include "executor/memory_pool.hpp"
#include "executor/task_runner.hpp"
#include "executor/thread_pool.hpp"
#include "network/async_transport.hpp"
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
#include <shared_mutex>
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
    size_t offload_fallbacks = 0;   ///< Offloads that failed over to local execution
    Duration total_duration{0};
};

/**
 * @brief Serialized task request/response for TCP offloading.
 *
 * Encodes/decodes edge_orchestrator.Envelope Protobuf messages
 * (proto/protocol.proto). The payload travels inside a length-prefixed
 * TcpTransport frame: [4B BE length][serialized Envelope].
 *
 * Protobuf stays quarantined inside offload_codec.cpp: no generated
 * header leaks into module interfaces.
 */
struct OffloadCodec {
    /// Which Envelope payload a frame carries (server-side dispatch).
    enum class WireKind : uint8_t {
        Unknown,
        OffloadRequest,
        OffloadResponse,
        WorkloadSubmission,
        WorkloadResult
    };

    static WireKind classify(const std::vector<uint8_t>& data);

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

    static std::vector<uint8_t> encode_submission(const std::string& workload_id,
                                                   const WorkloadDAG& dag);
    static bool decode_submission(const std::vector<uint8_t>& data,
                                  std::string& workload_id,
                                  WorkloadDAG& dag);

    static std::vector<uint8_t> encode_workload_result(const std::string& workload_id,
                                                        bool accepted,
                                                        const std::string& error_msg,
                                                        const OrchestrationResult& result);
    static bool decode_workload_result(const std::vector<uint8_t>& data,
                                       std::string& workload_id,
                                       bool& accepted,
                                       std::string& error_msg,
                                       OrchestrationResult& result);
};

/**
 * @brief The top-level Orchestrator that wires all modules together.
 *
 * Template parameters allow injecting MockMonitor for testing.
 */
template <typename MonitorT = LinuxMonitor>
class Orchestrator {
    static_assert(ResourceMonitorLike<MonitorT>,
                  "MonitorT must satisfy ResourceMonitorLike");

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
    struct RemoteOutcome {
        bool transport_ok = false;   ///< Round trip succeeded (peer reachable + response decoded)
        bool task_ok = false;        ///< Peer reported successful execution
        Duration duration{0};
        std::string error;
    };

    std::unique_ptr<ISchedulingPolicy> create_policy() const;
    void handle_message(const std::vector<uint8_t>& request,
                        std::vector<uint8_t>& response);
    void handle_offload_request(const std::vector<uint8_t>& request,
                                std::vector<uint8_t>& response);
    void handle_workload_submission(const std::vector<uint8_t>& request,
                                    std::vector<uint8_t>& response);
    RemoteOutcome offload_to_peer(const TaskId& task_id,
                                  const TaskProfile& profile,
                                  const NodeId& target);
    void resource_update_loop(std::stop_token stop);
    ExecutionResult run_task(const TaskId& task_id,
                             const TaskProfile& profile,
                             std::stop_token stop);
    void try_reset_arena();

    Config config_;
    Logger logger_;
    MonitorT monitor_;

    // Executor. Tasks execute under a shared lock on arena_mutex_; the
    // arena resets only under the exclusive lock, so a reset can never
    // invalidate memory a running task still holds.
    ThreadPool thread_pool_;
    MemoryPool memory_pool_;
    TaskRunner task_runner_;
    std::shared_mutex arena_mutex_;

    // Network. The server is async (epoll + coroutines) so a slow
    // workload submission cannot block a peer's offload request; the
    // offload client stays synchronous on executor worker threads.
    ClusterViewManager cluster_mgr_;
    PeerDiscovery discovery_;
    AsyncTransport offload_server_;
    std::jthread resource_thread_;   ///< Feeds monitor snapshots to discovery

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

    // Register discovery callbacks. The full PeerInfo is stored so the
    // offload client can resolve node id -> IP:port later.
    discovery_.on_peer_discovered([this](const PeerInfo& peer) {
        cluster_mgr_.update_peer(peer);
        logger_.info("Peer discovered: " + peer.node_id
                     + " at " + peer.address + ":" + std::to_string(peer.tcp_port));
        metrics_.record_peer_event(peer.node_id, "discovered");
    });

    discovery_.on_peer_lost([this](const PeerInfo& peer) {
        cluster_mgr_.remove_peer(peer.node_id);
        logger_.warn("Peer lost: " + peer.node_id);
        metrics_.record_peer_event(peer.node_id, "lost");
    });

    // Start offload server — dispatches on the Envelope payload type
    auto listen_result = offload_server_.listen(config_.node.port);
    if (listen_result.has_value()) {
        offload_server_.serve([this](const std::vector<uint8_t>& request) -> std::vector<uint8_t> {
            std::vector<uint8_t> response;
            handle_message(request, response);
            return response;
        });
        logger_.info("Offload server listening on port " + std::to_string(config_.node.port));
    } else {
        logger_.warn("Could not start offload server: " + listen_result.error().message);
    }

    // Start discovery
    discovery_.start();

    // Keep the advertised resource picture fresh
    resource_thread_ = std::jthread([this](std::stop_token stop) {
        resource_update_loop(stop);
    });

    logger_.info("Orchestrator started successfully");
    return Result<void>{};
}

template <typename MonitorT>
void Orchestrator<MonitorT>::stop() {
    if (!running_.exchange(false)) return;

    logger_.info("Orchestrator shutting down...");
    if (resource_thread_.joinable()) {
        resource_thread_.request_stop();
        resource_thread_.join();
    }
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
    std::atomic<size_t> fallbacks{0};
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
                auto exec_result = run_task(task_id, profile, token);
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
            thread_pool_.submit([this, &completed, &failed, &fallbacks,
                                  profile = task->profile,
                                  task_id = decision.task_id,
                                  target = decision.assigned_node]() {
                auto outcome = offload_to_peer(task_id, profile, target);

                if (outcome.transport_ok) {
                    metrics_.record_task_event(task_id,
                        outcome.task_ok ? TaskState::Completed : TaskState::Failed,
                        outcome.duration);
                    if (outcome.task_ok) {
                        completed.fetch_add(1);
                    } else {
                        logger_.warn("Peer " + target + " failed task " + task_id
                                     + ": " + outcome.error);
                        failed.fetch_add(1);
                    }
                    return;
                }

                // The peer was unreachable or the round trip broke: the task
                // must not be lost, so it fails over to local execution.
                logger_.warn("Offload of " + task_id + " to " + target
                             + " failed (" + outcome.error + "); executing locally");
                metrics_.record_custom("offload_fallback",
                    "{\"task\":\"" + task_id + "\",\"target\":\"" + target + "\"}");
                fallbacks.fetch_add(1);

                auto exec_result = run_task(task_id, profile, std::stop_token{});
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
    result.offload_fallbacks = fallbacks.load();

    // This workload's arena allocations are dead once its tasks are done.
    try_reset_arena();
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
typename Orchestrator<MonitorT>::RemoteOutcome
Orchestrator<MonitorT>::offload_to_peer(const TaskId& task_id,
                                        const TaskProfile& profile,
                                        const NodeId& target) {
    RemoteOutcome outcome;

    auto peer = cluster_mgr_.peer_info(target);
    if (!peer || peer->address.empty() || peer->tcp_port == 0) {
        outcome.error = "no known endpoint for " + target;
        return outcome;
    }

    TcpTransport client;
    auto conn = client.connect(peer->address, peer->tcp_port,
                               config_.network.offload_timeout_ms);
    if (!conn.has_value()) {
        outcome.error = "connect: " + conn.error().message;
        return outcome;
    }

    auto request = OffloadCodec::encode_request(task_id, profile);
    auto sent = client.send(request);
    if (!sent.has_value()) {
        outcome.error = "send: " + sent.error().message;
        return outcome;
    }

    auto reply = client.receive(config_.network.offload_timeout_ms);
    if (!reply.has_value()) {
        outcome.error = "receive: " + reply.error().message;
        return outcome;
    }

    bool success = false;
    Duration duration{0};
    uint64_t peak_memory = 0;
    std::string error_msg;
    std::vector<uint8_t> output;
    if (!OffloadCodec::decode_response(*reply, success, duration,
                                       peak_memory, error_msg, output)) {
        outcome.error = "malformed response";
        return outcome;
    }

    outcome.transport_ok = true;
    outcome.task_ok = success;
    outcome.duration = duration;
    outcome.error = error_msg;
    return outcome;
}

template <typename MonitorT>
ExecutionResult Orchestrator<MonitorT>::run_task(const TaskId& task_id,
                                                 const TaskProfile& profile,
                                                 std::stop_token stop) {
    std::shared_lock guard(arena_mutex_);
    return task_runner_.execute(task_id, profile, memory_pool_, std::move(stop));
}

template <typename MonitorT>
void Orchestrator<MonitorT>::try_reset_arena() {
    // The arena only frees wholesale. Reset opportunistically at task/
    // workload boundaries: the exclusive lock guarantees quiescence, and
    // try_to_lock means a busy executor just defers to the next boundary.
    std::unique_lock guard(arena_mutex_, std::try_to_lock);
    if (guard.owns_lock()) {
        memory_pool_.reset();
    }
}

template <typename MonitorT>
void Orchestrator<MonitorT>::resource_update_loop(std::stop_token stop) {
    const auto interval = std::chrono::milliseconds(
        config_.monitor.sampling_interval_ms == 0 ? 500
                                                  : config_.monitor.sampling_interval_ms);
    while (!stop.stop_requested()) {
        auto snap = monitor_.read();
        if (snap.has_value()) {
            discovery_.update_local_resources(*snap);
        }

        auto deadline = std::chrono::steady_clock::now() + interval;
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

template <typename MonitorT>
void Orchestrator<MonitorT>::handle_message(
    const std::vector<uint8_t>& request,
    std::vector<uint8_t>& response) {

    switch (OffloadCodec::classify(request)) {
        case OffloadCodec::WireKind::OffloadRequest:
            handle_offload_request(request, response);
            break;
        case OffloadCodec::WireKind::WorkloadSubmission:
            handle_workload_submission(request, response);
            break;
        default:
            logger_.warn("Received unsupported message ("
                         + std::to_string(request.size()) + " bytes)");
            response = OffloadCodec::encode_response(false, Duration{0}, 0,
                                                      "Unsupported message type");
            break;
    }
}

template <typename MonitorT>
void Orchestrator<MonitorT>::handle_workload_submission(
    const std::vector<uint8_t>& request,
    std::vector<uint8_t>& response) {

    std::string workload_id;
    WorkloadDAG dag;
    if (!OffloadCodec::decode_submission(request, workload_id, dag)) {
        response = OffloadCodec::encode_workload_result(
            workload_id, false, "Invalid workload submission", {});
        return;
    }

    logger_.info("Workload '" + workload_id + "' received ("
                 + std::to_string(dag.task_count()) + " tasks)");

    auto result = submit_workload(dag);
    if (!result.has_value()) {
        response = OffloadCodec::encode_workload_result(
            workload_id, false, result.error().message, {});
        return;
    }

    response = OffloadCodec::encode_workload_result(workload_id, true, "", *result);
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

    auto exec_result = run_task(task_id, profile, std::stop_token{});

    bool success = (exec_result.final_state == TaskState::Completed);
    std::string err = exec_result.error_message.value_or("");

    response = OffloadCodec::encode_response(success, exec_result.actual_duration,
                                              exec_result.peak_memory_bytes, err);

    metrics_.record_task_event(task_id, exec_result.final_state,
                                exec_result.actual_duration);
    try_reset_arena();
}

}  // namespace edge_orchestrator
