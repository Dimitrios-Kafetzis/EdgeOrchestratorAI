/**
 * @file bench_scheduler.cpp
 * @brief Performance benchmarks for scheduling policies and core operations.
 * @author Dimitris Kafetzis
 *
 * Measures scheduling overhead, DAG operations, and transport throughput
 * to provide empirical data for the PhD thesis evaluation.
 *
 * Usage: ./bench_scheduler [--csv]
 *        ./bench_scheduler --sweep   (policy campaign: topology × DAG size ×
 *                                     cluster size × policy, CSV to stdout)
 */

#include "core/config.hpp"
#include "core/types.hpp"
#include "executor/memory_pool.hpp"
#include "executor/task_runner.hpp"
#include "executor/thread_pool.hpp"
#include "network/cluster_view.hpp"
#include "network/transport.hpp"
#include "resource_monitor/monitor.hpp"
#include "scheduler/greedy_policy.hpp"
#include "scheduler/threshold_policy.hpp"
#include "scheduler/optimizer_policy.hpp"
#include "workload/dag.hpp"
#include "workload/generator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using namespace edge_orchestrator;
using Clock = std::chrono::high_resolution_clock;

// ─────────────────────────────────────────────
// Benchmark Harness
// ─────────────────────────────────────────────

struct BenchResult {
    std::string name;
    std::string category;
    double mean_us;
    double stddev_us;
    double min_us;
    double max_us;
    double p99_us;
    size_t iterations;
    std::string extra;
};

template <typename Fn>
BenchResult run_bench(const std::string& name,
                      const std::string& category,
                      size_t iterations,
                      Fn&& fn,
                      const std::string& extra = "") {
    std::vector<double> timings;
    timings.reserve(iterations);

    // Warmup
    for (size_t i = 0; i < std::min(iterations / 10, size_t{5}); ++i) fn();

    for (size_t i = 0; i < iterations; ++i) {
        auto start = Clock::now();
        fn();
        auto end = Clock::now();
        timings.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }

    std::sort(timings.begin(), timings.end());

    double sum = std::accumulate(timings.begin(), timings.end(), 0.0);
    double mean = sum / static_cast<double>(iterations);
    double sq_sum = std::accumulate(timings.begin(), timings.end(), 0.0,
        [mean](double acc, double v) { return acc + (v - mean) * (v - mean); });
    double stddev = std::sqrt(sq_sum / static_cast<double>(iterations));

    size_t p99_idx = std::min(static_cast<size_t>(0.99 * static_cast<double>(iterations)),
                              iterations - 1);

    return BenchResult{
        .name = name, .category = category,
        .mean_us = mean, .stddev_us = stddev,
        .min_us = timings.front(), .max_us = timings.back(),
        .p99_us = timings[p99_idx], .iterations = iterations, .extra = extra
    };
}

void print_results(const std::vector<BenchResult>& results, bool csv) {
    if (csv) {
        std::cout << "category,name,mean_us,stddev_us,min_us,max_us,p99_us,iterations,extra\n";
        for (const auto& r : results) {
            std::cout << r.category << "," << r.name << ","
                      << std::fixed << std::setprecision(2)
                      << r.mean_us << "," << r.stddev_us << ","
                      << r.min_us << "," << r.max_us << "," << r.p99_us << ","
                      << r.iterations << "," << r.extra << "\n";
        }
        return;
    }

    std::string current_cat;
    for (const auto& r : results) {
        if (r.category != current_cat) {
            current_cat = r.category;
            std::cout << "\n══ " << current_cat << " ══\n";
            std::cout << std::left << std::setw(42) << "Benchmark"
                      << std::right << std::setw(11) << "Mean(us)"
                      << std::setw(11) << "Stddev"
                      << std::setw(11) << "P99(us)"
                      << std::setw(11) << "Min(us)"
                      << "  Info\n"
                      << std::string(98, '-') << "\n";
        }
        std::cout << std::left << std::setw(42) << r.name
                  << std::right << std::fixed << std::setprecision(1)
                  << std::setw(11) << r.mean_us
                  << std::setw(11) << r.stddev_us
                  << std::setw(11) << r.p99_us
                  << std::setw(11) << r.min_us
                  << "  " << r.extra << "\n";
    }
}

// ─────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────

ResourceSnapshot make_snap(const std::string& id = "local", float cpu = 30.0f) {
    ResourceSnapshot s;
    s.node_id = id;
    s.cpu_usage_percent = cpu;
    s.memory_available_bytes = 3ULL << 30;
    s.memory_total_bytes = 4ULL << 30;
    s.timestamp = std::chrono::system_clock::now();
    return s;
}

ClusterView make_cluster(size_t n) {
    ClusterView v;
    for (size_t i = 0; i < n; ++i) {
        auto id = "peer-" + std::to_string(i);
        PeerInfo p{.node_id = id, .resources = make_snap(id, 20.0f + float(i) * 10),
                   .address = "127.0.0.1", .tcp_port = 0, .reachable = true};
        v.peers.push_back(p);
    }
    return v;
}

// ─────────────────────────────────────────────
// Suites
// ─────────────────────────────────────────────

std::vector<BenchResult> bench_dag() {
    std::vector<BenchResult> R;
    constexpr size_t N = 1000;
    TaskProfile pr{.compute_cost = Duration{1000}, .memory_bytes = 4096};

    for (size_t n : {10u, 50u, 100u, 200u})
        R.push_back(run_bench("linear_chain(" + std::to_string(n) + ")", "DAG Generation", N,
            [&]{ auto d = WorkloadGenerator::linear_chain(n, pr); (void)d; },
            std::to_string(n) + " tasks"));

    for (size_t L : {4u, 12u, 24u, 48u})
        R.push_back(run_bench("transformer(" + std::to_string(L) + "L)", "DAG Generation", N,
            [&]{ auto d = WorkloadGenerator::transformer_layers(L, 768, 2048); (void)d; },
            std::to_string(L * 2) + " tasks"));

    auto d50 = WorkloadGenerator::linear_chain(50, pr);
    auto dt12 = WorkloadGenerator::transformer_layers(12, 768, 2048);

    R.push_back(run_bench("topo_order(50)", "DAG Analysis", N,
        [&]{ auto o = d50.topological_order(); (void)o; }, "50 tasks"));
    R.push_back(run_bench("critical_path(50)", "DAG Analysis", N,
        [&]{ auto c = d50.critical_path_cost(); (void)c; }, "50 tasks"));
    R.push_back(run_bench("critical_path(transformer12)", "DAG Analysis", N,
        [&]{ auto c = dt12.critical_path_cost(); (void)c; }, "24 tasks"));
    R.push_back(run_bench("has_cycle(50)", "DAG Analysis", N,
        [&]{ auto c = d50.has_cycle(); (void)c; }, "50 tasks"));

    return R;
}

std::vector<BenchResult> bench_scheduling() {
    std::vector<BenchResult> R;
    auto snap = make_snap();
    ClusterView empty;
    TaskProfile pr{.compute_cost = Duration{1000}, .memory_bytes = 4096};

    for (size_t n : {10u, 50u, 100u}) {
        auto dag = WorkloadGenerator::linear_chain(n, pr);
        auto label = std::to_string(n) + "T, 0 peers";

        R.push_back(run_bench("greedy_1node(" + std::to_string(n) + ")", "Scheduling", 500,
            [&]{ GreedyPolicy p; auto r = p.schedule(dag, snap, empty); (void)r; }, label));
        R.push_back(run_bench("threshold_1node(" + std::to_string(n) + ")", "Scheduling", 500,
            [&]{ ThresholdPolicy p({}); auto r = p.schedule(dag, snap, empty); (void)r; }, label));
        R.push_back(run_bench("optimizer_1node(" + std::to_string(n) + ")", "Scheduling", 100,
            [&]{ OptimizerPolicy p({}); auto r = p.schedule(dag, snap, empty); (void)r; }, label));
    }

    for (size_t peers : {1u, 3u, 5u}) {
        auto cluster = make_cluster(peers);
        auto dag = WorkloadGenerator::transformer_layers(12, 768, 2048);
        auto label = "24T, " + std::to_string(peers) + " peers";

        R.push_back(run_bench("greedy_" + std::to_string(peers+1) + "node(t12)", "Scheduling", 500,
            [&]{ GreedyPolicy p; auto r = p.schedule(dag, snap, cluster); (void)r; }, label));
        R.push_back(run_bench("optimizer_" + std::to_string(peers+1) + "node(t12)", "Scheduling", 100,
            [&]{ OptimizerPolicy p({}); auto r = p.schedule(dag, snap, cluster); (void)r; }, label));
    }

    return R;
}

std::vector<BenchResult> bench_monitor() {
    std::vector<BenchResult> R;
    constexpr size_t N = 1000;

    MockMonitor mock("bench"); mock.start();
    R.push_back(run_bench("mock_read", "Monitor", N, [&]{ auto s = mock.read(); (void)s; }));
    mock.stop();

    LinuxMonitor lm("bench", 50); lm.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    R.push_back(run_bench("linux_read", "Monitor", N, [&]{ auto s = lm.read(); (void)s; }));
    lm.stop();

    ClusterViewManager mgr;
    for (int i = 0; i < 10; ++i) mgr.update_peer("p" + std::to_string(i), make_snap("p" + std::to_string(i)));
    R.push_back(run_bench("cluster_snapshot(10)", "Monitor", N, [&]{ auto v = mgr.snapshot(); (void)v; }));
    R.push_back(run_bench("cluster_update", "Monitor", N, [&]{ mgr.update_peer("p0", make_snap("p0")); }));

    return R;
}

std::vector<BenchResult> bench_executor() {
    std::vector<BenchResult> R;
    constexpr size_t N = 500;

    MemoryPool pool(64 << 20);
    R.push_back(run_bench("mempool_alloc_4KB", "Executor", N,
        [&]{ auto r = pool.allocate(4096); (void)r; pool.reset(); }));
    R.push_back(run_bench("mempool_alloc_1MB", "Executor", N,
        [&]{ auto r = pool.allocate(1 << 20); (void)r; pool.reset(); }));

    MemoryPool tp(4 << 20);
    TaskRunner runner;
    std::stop_source ss;
    TaskProfile tiny{.compute_cost = Duration{1}, .memory_bytes = 64};
    R.push_back(run_bench("task_exec_overhead", "Executor", N,
        [&]{ tp.reset(); runner.execute("b", tiny, tp, ss.get_token()); }));

    ThreadPool tpool(4);
    R.push_back(run_bench("threadpool_submit", "Executor", N, [&]{
        std::promise<void> p; auto f = p.get_future();
        tpool.submit([&p]{ p.set_value(); }); f.wait();
    }));

    return R;
}

std::vector<BenchResult> bench_transport() {
    std::vector<BenchResult> R;
    TcpTransport server;
    if (!server.listen(19990).has_value()) return R;
    server.serve([](const std::vector<uint8_t>& r){ return r; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TcpTransport client;
    if (!client.connect("127.0.0.1", 19990, 2000).has_value()) { server.stop_serving(); return R; }

    std::vector<uint8_t> small(64), large(64 * 1024);
    R.push_back(run_bench("tcp_roundtrip_64B", "Transport", 200,
        [&]{ client.send(small); auto r = client.receive(5000); (void)r; }, "64 B"));

    client.disconnect();
    client.connect("127.0.0.1", 19990, 2000);
    R.push_back(run_bench("tcp_roundtrip_64KB", "Transport", 100,
        [&]{ client.send(large); auto r = client.receive(5000); (void)r; }, "64 KB"));

    client.disconnect();
    server.stop_serving();
    return R;
}

// ─────────────────────────────────────────────
// Policy Sweep (--sweep)
//
// Campaign: topology × DAG size × cluster size × policy.
// Measures scheduling latency and evaluates each policy's plan under a
// single dependency-aware makespan model (the same list-scheduling model
// the optimizer uses internally), so makespan numbers are comparable
// across policies rather than each policy grading its own homework.
// ─────────────────────────────────────────────

/**
 * @brief Dependency-aware makespan of a plan, independent of policy.
 *
 * List schedule in topological order: one execution slot per node,
 * cross-node edges pay output_bytes × comm_weight × 0.001 us — identical
 * to OptimizerPolicy's internal model.
 */
double evaluate_plan_makespan_ms(const WorkloadDAG& dag,
                                 const SchedulingPlan& plan,
                                 const ResourceSnapshot& local,
                                 const ClusterView& cluster,
                                 float comm_weight = 0.3f) {
    std::unordered_map<std::string, size_t> node_index;
    node_index[local.node_id] = 0;
    size_t next = 1;
    for (const auto& peer : cluster.peers) node_index[peer.node_id] = next++;

    std::unordered_map<TaskId, size_t> assignment;
    for (const auto& d : plan.decisions) {
        auto it = node_index.find(d.assigned_node);
        if (it != node_index.end()) assignment[d.task_id] = it->second;
    }

    std::unordered_map<TaskId, int64_t> end_time;
    std::vector<int64_t> node_free(node_index.size(), 0);
    int64_t makespan = 0;

    for (const auto& task_id : dag.topological_order()) {
        auto task = dag.get_task(task_id);
        if (!task) continue;
        auto assigned = assignment.find(task_id);
        if (assigned == assignment.end()) continue;
        size_t node = assigned->second;

        int64_t ready = 0;
        for (const auto& dep_id : dag.dependencies(task_id)) {
            auto dep_end = end_time.find(dep_id);
            if (dep_end == end_time.end()) continue;
            int64_t arrival = dep_end->second;
            auto dep_assigned = assignment.find(dep_id);
            if (dep_assigned != assignment.end() && dep_assigned->second != node) {
                auto dep_task = dag.get_task(dep_id);
                if (dep_task) {
                    arrival += static_cast<int64_t>(
                        static_cast<float>(dep_task->profile.output_bytes)
                        * comm_weight * 0.001f);
                }
            }
            ready = std::max(ready, arrival);
        }

        int64_t start = std::max(ready, node_free[node]);
        int64_t end = start + task->profile.compute_cost.count();
        end_time[task_id] = end;
        node_free[node] = end;
        makespan = std::max(makespan, end);
    }

    return static_cast<double>(makespan) / 1000.0;  // us → ms
}

WorkloadDAG make_sweep_dag(const std::string& topology, size_t tasks) {
    TaskProfile pr{.compute_cost = Duration{1000}, .memory_bytes = 4096,
                   .input_bytes = 64 * 1024, .output_bytes = 64 * 1024};
    if (topology == "chain") return WorkloadGenerator::linear_chain(tasks, pr);
    if (topology == "fanout") return WorkloadGenerator::fan_out_fan_in(tasks - 2, pr);
    if (topology == "transformer")
        return WorkloadGenerator::transformer_layers(tasks / 2, 768, 2048);
    // random: fixed seed for reproducibility across runs and machines
    std::mt19937 rng(12345);
    TaskProfile lo{.compute_cost = Duration{500}, .memory_bytes = 4096,
                   .input_bytes = 4 * 1024, .output_bytes = 4 * 1024};
    TaskProfile hi{.compute_cost = Duration{2000}, .memory_bytes = 1 << 20,
                   .input_bytes = 64 * 1024, .output_bytes = 64 * 1024};
    return WorkloadGenerator::random_dag(tasks, 0.1f, lo, hi, rng);
}

void run_sweep() {
    auto local = make_snap();
    std::cout << "topology,tasks,nodes,policy,sched_mean_us,sched_stddev_us,"
                 "sched_p99_us,sched_min_us,iterations,makespan_ms,local_fraction\n";

    for (const std::string topology : {"chain", "fanout", "random", "transformer"}) {
        for (size_t tasks : {10u, 50u, 100u, 500u}) {
            auto dag = make_sweep_dag(topology, tasks);
            for (size_t nodes : {1u, 3u, 5u}) {
                auto cluster = make_cluster(nodes - 1);
                for (const std::string policy_name : {"greedy", "threshold", "optimizer"}) {
                    auto make_policy = [&]() -> std::unique_ptr<ISchedulingPolicy> {
                        if (policy_name == "greedy") return std::make_unique<GreedyPolicy>();
                        if (policy_name == "threshold")
                            return std::make_unique<ThresholdPolicy>(ThresholdPolicyConfig{});
                        return std::make_unique<OptimizerPolicy>(OptimizerPolicyConfig{});
                    };

                    // Fewer reps where a single call is expensive
                    size_t reps = (policy_name == "optimizer")
                                      ? (tasks >= 500 ? 30 : 100)
                                      : (tasks >= 500 ? 100 : 300);

                    auto res = run_bench(policy_name, "Sweep", reps, [&] {
                        auto p = make_policy();
                        auto plan = p->schedule(dag, local, cluster);
                        (void)plan;
                    });

                    auto plan = make_policy()->schedule(dag, local, cluster);
                    double makespan_ms =
                        evaluate_plan_makespan_ms(dag, plan, local, cluster);
                    size_t local_count = 0;
                    for (const auto& d : plan.decisions)
                        if (d.assigned_node == local.node_id) ++local_count;
                    double local_fraction = plan.decisions.empty()
                        ? 1.0
                        : static_cast<double>(local_count)
                              / static_cast<double>(plan.decisions.size());

                    std::cout << topology << "," << tasks << "," << nodes << ","
                              << policy_name << "," << std::fixed
                              << std::setprecision(2) << res.mean_us << ","
                              << res.stddev_us << "," << res.p99_us << ","
                              << res.min_us << "," << reps << ","
                              << std::setprecision(3) << makespan_ms << ","
                              << local_fraction << "\n";
                }
            }
        }
    }
}

int main(int argc, char* argv[]) {
    bool csv = (argc > 1 && std::strcmp(argv[1], "--csv") == 0);

    if (argc > 1 && std::strcmp(argv[1], "--sweep") == 0) {
        run_sweep();
        return 0;
    }

    if (!csv) {
        std::cout << "\n  EdgeOrchestrator Performance Benchmarks\n"
                  << "  " << std::string(40, '=') << "\n"
                  << "  Platform: " << sizeof(void*) * 8 << "-bit, "
                  << std::thread::hardware_concurrency() << " cores\n";
    }

    std::vector<BenchResult> all;
    auto append = [&](auto&& v){ all.insert(all.end(), v.begin(), v.end()); };

    append(bench_dag());
    append(bench_scheduling());
    append(bench_monitor());
    append(bench_executor());
    append(bench_transport());

    print_results(all, csv);
    if (!csv) std::cout << "\n  Total: " << all.size() << " benchmarks\n\n";
    return 0;
}
