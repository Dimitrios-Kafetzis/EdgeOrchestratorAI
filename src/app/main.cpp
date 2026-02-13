/**
 * @file main.cpp
 * @brief EdgeOrchestrator daemon entry point.
 * @author Dimitris Kafetzis
 *
 * Wires all modules into a complete orchestration pipeline:
 *   Config → Logger → Monitor → Discovery → ClusterView → Scheduler → Executor → Telemetry
 */

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
#include "workload/dag.hpp"
#include "workload/generator.hpp"

#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

using namespace edge_orchestrator;

namespace {

volatile std::sig_atomic_t g_shutdown_requested = 0;

void signal_handler(int /*signal*/) {
    g_shutdown_requested = 1;
}

void print_banner() {
    std::cout << R"(
  ╔═══════════════════════════════════════════╗
  ║         EdgeOrchestrator v1.0.0           ║
  ║   Adaptive Resource Orchestrator for      ║
  ║   Edge AI Workload Scheduling             ║
  ╚═══════════════════════════════════════════╝
)" << std::endl;
}

struct CLIArgs {
    std::filesystem::path config_path = "config/default.toml";
    std::string node_id;
    uint16_t port = 0;
    std::string log_dir;
    bool demo_mode = false;
};

CLIArgs parse_args(int argc, char* argv[]) {
    CLIArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            args.config_path = argv[++i];
        } else if (arg == "--node-id" && i + 1 < argc) {
            args.node_id = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            args.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--log-dir" && i + 1 < argc) {
            args.log_dir = argv[++i];
        } else if (arg == "--demo") {
            args.demo_mode = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: edge_orchestrator [OPTIONS]\n"
                      << "  --config <path>    Configuration file (default: config/default.toml)\n"
                      << "  --node-id <id>     Node identifier\n"
                      << "  --port <port>      TCP port for inter-node communication\n"
                      << "  --log-dir <path>   Log output directory\n"
                      << "  --demo             Run a single scheduling demo, then exit\n"
                      << "  --help, -h         Show this help message\n";
            std::exit(0);
        }
    }
    return args;
}

/**
 * @brief Execute a scheduling plan on the local executor.
 */
void execute_plan(const SchedulingPlan& plan,
                  const WorkloadDAG& dag,
                  const NodeId& local_id,
                  ThreadPool& pool,
                  MemoryPool& mem_pool,
                  TaskRunner& runner,
                  MetricsCollector& metrics,
                  Logger& logger) {
    std::stop_source stop_source;
    size_t local_executed = 0;
    size_t remote_offloaded = 0;

    for (const auto& decision : plan.decisions) {
        if (g_shutdown_requested) break;

        auto task = dag.get_task(decision.task_id);
        if (!task) continue;

        if (decision.assigned_node == local_id) {
            pool.submit([&, task_id = decision.task_id,
                         profile = task->profile,
                         stop_token = stop_source.get_token()]() {
                auto result = runner.execute(task_id, profile, mem_pool, stop_token);
                metrics.record_task_event(task_id, result.final_state,
                                          result.actual_duration);
                if (result.final_state == TaskState::Completed) {
                    logger.debug("Task " + task_id + " completed in "
                                 + std::to_string(result.actual_duration.count()) + "us");
                } else if (result.error_message) {
                    logger.warn("Task " + task_id + " failed: " + *result.error_message);
                }
            });
            ++local_executed;
        } else {
            logger.info("Offload task " + decision.task_id
                        + " -> " + decision.assigned_node);
            metrics.record_scheduling_decision(decision);
            ++remote_offloaded;
        }
    }

    metrics.record_custom("plan_summary",
        "{\"local\":" + std::to_string(local_executed)
        + ",\"offloaded\":" + std::to_string(remote_offloaded)
        + ",\"makespan_us\":" + std::to_string(plan.estimated_makespan.count()) + "}");

    logger.info("Plan executed: " + std::to_string(local_executed) + " local, "
                + std::to_string(remote_offloaded) + " offloaded, makespan "
                + std::to_string(plan.estimated_makespan.count()) + "us");
}

/**
 * @brief Run a single demo: generate a transformer workload, schedule it,
 *        execute locally, and print results.
 */
void run_demo(const Config& config, Logger& logger) {
    logger.info("=== Demo Mode ===");

    // Generate a 6-layer transformer workload
    auto dag = WorkloadGenerator::transformer_layers(6, 768, 2048);
    logger.info("Generated workload: " + std::to_string(dag.task_count()) + " tasks, "
                + "critical path " + std::to_string(dag.critical_path_cost().count()) + "us, "
                + "peak memory " + std::to_string(dag.peak_memory_estimate() / 1024) + " KB");

    // Set up mock monitor for demo
    MockMonitor monitor(config.node.id);
    monitor.set_cpu(35.0f);
    monitor.set_memory(3ULL * 1024 * 1024 * 1024, 4ULL * 1024 * 1024 * 1024);
    monitor.start();
    auto snap = monitor.read();

    // Set up cluster with a simulated peer
    ClusterViewManager cluster_mgr;
    ResourceSnapshot peer_snap;
    peer_snap.node_id = "rpi-peer-1";
    peer_snap.cpu_usage_percent = 15.0f;
    peer_snap.memory_available_bytes = 3ULL * 1024 * 1024 * 1024;
    peer_snap.memory_total_bytes = 4ULL * 1024 * 1024 * 1024;
    cluster_mgr.update_peer("rpi-peer-1", peer_snap);

    auto cluster_view = cluster_mgr.snapshot();

    // Schedule with all three policies and compare
    GreedyPolicy greedy;
    ThresholdPolicy threshold(config.scheduler.threshold);
    OptimizerPolicy optimizer(config.scheduler.optimizer);

    auto plan_g = greedy.schedule(dag, *snap, cluster_view);
    auto plan_t = threshold.schedule(dag, *snap, cluster_view);
    auto plan_o = optimizer.schedule(dag, *snap, cluster_view);

    auto count_local = [&](const SchedulingPlan& plan) {
        size_t n = 0;
        for (const auto& d : plan.decisions)
            if (d.assigned_node == config.node.id) ++n;
        return n;
    };

    logger.info("Greedy:    " + std::to_string(count_local(plan_g)) + " local, "
                + std::to_string(plan_g.decisions.size() - count_local(plan_g)) + " offloaded, "
                + "makespan " + std::to_string(plan_g.estimated_makespan.count()) + "us");
    logger.info("Threshold: " + std::to_string(count_local(plan_t)) + " local, "
                + std::to_string(plan_t.decisions.size() - count_local(plan_t)) + " offloaded, "
                + "makespan " + std::to_string(plan_t.estimated_makespan.count()) + "us");
    logger.info("Optimizer: " + std::to_string(count_local(plan_o)) + " local, "
                + std::to_string(plan_o.decisions.size() - count_local(plan_o)) + " offloaded, "
                + "makespan " + std::to_string(plan_o.estimated_makespan.count()) + "us");

    // Execute the optimizer plan locally
    auto thread_count = config.executor.thread_count == 0
        ? std::thread::hardware_concurrency() : config.executor.thread_count;
    ThreadPool pool(thread_count);
    MemoryPool mem_pool(config.executor.memory_pool_mb * 1024 * 1024);
    TaskRunner runner;
    auto metrics_sink = std::make_unique<NullSink>();
    MetricsCollector metrics(std::move(metrics_sink));

    execute_plan(plan_o, dag, config.node.id, pool, mem_pool, runner, metrics, logger);

    // Wait for tasks to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    logger.info("=== Demo Complete ===");
    monitor.stop();
}

}  // namespace

int main(int argc, char* argv[]) {
    print_banner();

    auto args = parse_args(argc, argv);

    // Load configuration
    auto config_result = load_config(args.config_path);
    if (!config_result) {
        std::cerr << "Failed to load config: " << config_result.error().message << std::endl;
        std::cerr << "Using default configuration." << std::endl;
    }
    auto config = config_result ? *config_result : default_config();

    // Apply CLI overrides
    if (!args.node_id.empty()) config.node.id = args.node_id;
    if (args.port != 0) config.node.port = args.port;
    if (!args.log_dir.empty()) config.telemetry.log_dir = args.log_dir;

    // ── Initialize Logger ────────────────────
    std::unique_ptr<ILogSink> log_sink;
    if (!config.telemetry.log_dir.empty()) {
        log_sink = std::make_unique<JsonFileSink>(config.telemetry.log_dir, "edge_orchestrator");
    } else {
        log_sink = std::make_unique<StdoutSink>();
    }
    Logger logger(std::move(log_sink), LogLevel::Info);
    logger.info("EdgeOrchestrator starting...");
    logger.info("Node ID: " + config.node.id);
    logger.info("Port: " + std::to_string(config.node.port));
    logger.info("Scheduler policy: " + config.scheduler.policy);

    // Register signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── Demo mode shortcut ───────────────────
    if (args.demo_mode) {
        run_demo(config, logger);
        return 0;
    }

    // ── Initialize Executor ──────────────────
    auto thread_count = config.executor.thread_count == 0
        ? std::thread::hardware_concurrency()
        : config.executor.thread_count;
    ThreadPool pool(thread_count);
    MemoryPool mem_pool(config.executor.memory_pool_mb * 1024 * 1024);
    // TaskRunner is available for workload execution via execute_plan()
    logger.info("Executor: " + std::to_string(thread_count) + " threads, "
                + std::to_string(config.executor.memory_pool_mb) + " MB pool");

    // ── Initialize Resource Monitor ──────────
    LinuxMonitor monitor(config.node.id, config.monitor.sampling_interval_ms);
    monitor.on_cpu_threshold(90.0f, [&logger](const ResourceSnapshot&) {
        logger.warn("CPU usage exceeded 90%");
    });
    monitor.on_memory_threshold(85.0f, [&logger](const ResourceSnapshot&) {
        logger.warn("Memory usage exceeded 85%");
    });
    monitor.start();
    logger.info("Resource monitor started (interval: "
                + std::to_string(config.monitor.sampling_interval_ms) + "ms)");

    // ── Initialize Network Layer ─────────────
    ClusterViewManager cluster_mgr;

    PeerDiscovery discovery(config.node.id,
                            config.node.port,
                            config.network.heartbeat_interval_ms,
                            config.network.peer_timeout_ms);

    discovery.on_peer_discovered([&](const PeerInfo& peer) {
        cluster_mgr.update_peer(peer.node_id, peer.resources);
        logger.info("Peer discovered: " + peer.node_id
                    + " (CPU " + std::to_string(static_cast<int>(peer.resources.cpu_usage_percent))
                    + "%, mem " + std::to_string(peer.resources.memory_available_bytes / (1024*1024))
                    + " MB)");
    });

    discovery.on_peer_lost([&](const PeerInfo& peer) {
        cluster_mgr.remove_peer(peer.node_id);
        logger.warn("Peer lost: " + peer.node_id);
    });

    // Set up offload server
    TcpTransport offload_server;
    auto listen_result = offload_server.listen(config.node.port);
    if (listen_result.has_value()) {
        offload_server.serve([&](const std::vector<uint8_t>& request) -> std::vector<uint8_t> {
            logger.info("Received offload request (" + std::to_string(request.size()) + " bytes)");
            return request;
        });
        logger.info("Offload server listening on port " + std::to_string(config.node.port));
    } else {
        logger.warn("Could not start offload server: " + listen_result.error().message);
    }

    discovery.start();
    logger.info("Peer discovery started (heartbeat: "
                + std::to_string(config.network.heartbeat_interval_ms) + "ms, timeout: "
                + std::to_string(config.network.peer_timeout_ms) + "ms)");

    // ── Initialize Telemetry ─────────────────
    auto telemetry_sink = std::make_unique<NullSink>();
    MetricsCollector metrics(std::move(telemetry_sink));
    logger.info("Telemetry collector initialized");

    // ── Main Scheduling Loop ─────────────────
    logger.info("Entering main loop. Press Ctrl+C to shutdown.");

    uint64_t loop_count = 0;
    while (!g_shutdown_requested) {
        auto snap_result = monitor.read();
        if (snap_result.has_value()) {
            discovery.update_local_resources(*snap_result);

            // Periodic status logging (every 30 seconds at 100ms intervals)
            if (loop_count % 300 == 0 && loop_count > 0) {
                auto cluster = cluster_mgr.snapshot();
                metrics.record_resource_snapshot(*snap_result);
                logger.info("Status: CPU "
                    + std::to_string(static_cast<int>(snap_result->cpu_usage_percent))
                    + "%, mem "
                    + std::to_string(snap_result->memory_available_bytes / (1024*1024))
                    + " MB avail, "
                    + std::to_string(cluster.available_count()) + " peers");
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ++loop_count;
    }

    // ── Graceful Shutdown ────────────────────
    logger.info("Shutdown requested. Cleaning up...");
    discovery.stop();
    offload_server.stop_serving();
    monitor.stop();

    logger.info("EdgeOrchestrator stopped.");
    return 0;
}
