# EdgeOrchestrator — API Reference

Quick reference for all public interfaces. See header files for full Doxygen documentation.

## Core Types (`core/types.hpp`)

```cpp
using NodeId   = std::string;
using Duration = std::chrono::microseconds;
using Timestamp = std::chrono::system_clock::time_point;

struct TaskProfile {
    Duration  compute_cost{0};        // Expected execution time
    uint64_t  memory_bytes    = 0;    // Working memory requirement
    uint64_t  input_bytes     = 0;    // Input data size
    uint64_t  output_bytes    = 0;    // Output data size
    auto operator<=>(const TaskProfile&) const = default;
};

struct ResourceSnapshot {
    NodeId    node_id;
    Timestamp timestamp;
    float     cpu_usage_percent          = 0.0f;
    std::array<float, 4> per_core_cpu_percent{};
    uint64_t  memory_available_bytes     = 0;
    uint64_t  memory_total_bytes         = 0;
    float     cpu_temperature_celsius    = 0.0f;
    float     gpu_temperature_celsius    = 0.0f;
    uint64_t  network_rx_bytes_sec       = 0;
    uint64_t  network_tx_bytes_sec       = 0;
    bool      is_throttled               = false;

    constexpr float memory_usage_percent() const noexcept;
    constexpr float cpu_headroom_percent() const noexcept;  // 100.0 - cpu_usage
};

enum class TaskState : uint8_t { Pending, Ready, Scheduled, Running, Completed, Failed };
```

## Result Monad (`core/result.hpp`)

```cpp
template <typename T, typename E = Error>
class Result {
    bool has_value() const;
    T& operator*();
    E& error();
    T value_or(T default_val);

    template <typename F> auto map(F&& f);       // Transform value
    template <typename F> auto and_then(F&& f);   // Chain operations
};

// Void specialization
template <> class Result<void, Error>;
```

## Concepts (`core/concepts.hpp`)

```cpp
template <typename T>
concept ResourceMonitorLike = requires(T monitor) {
    { monitor.read() } -> std::same_as<Result<ResourceSnapshot>>;
    { monitor.cpu_usage() } -> std::convertible_to<float>;
    { monitor.memory_available() } -> std::convertible_to<uint64_t>;
    { monitor.is_throttled() } -> std::convertible_to<bool>;
    { monitor.start() } -> std::same_as<void>;
    { monitor.stop() } -> std::same_as<void>;
};

template <typename T>
concept SchedulingPolicyLike = requires(T policy,
                                        const WorkloadDAG& dag,
                                        const ResourceSnapshot& local,
                                        const ClusterView& cluster) {
    { policy.schedule(dag, local, cluster) } -> std::same_as<SchedulingPlan>;
    { policy.name() } -> std::convertible_to<std::string_view>;
};

// Enforced in tests: static_assert(SchedulingPolicyLike<GreedyPolicy>) etc.,
// and Orchestrator static_asserts ResourceMonitorLike<MonitorT>.
```

## Resource Monitor (`resource_monitor/monitor.hpp`)

```cpp
class LinuxMonitor {    // Satisfies ResourceMonitorLike
    LinuxMonitor(NodeId node_id, uint32_t sampling_interval_ms = 500);
    void start();
    void stop();
    Result<ResourceSnapshot> read();

    void on_cpu_threshold(float percent, ThresholdCallback cb);
    void on_memory_threshold(float percent, ThresholdCallback cb);
};

class MockMonitor {     // Satisfies ResourceMonitorLike
    MockMonitor(NodeId node_id, uint32_t sampling_interval_ms = 0);
    void start();
    void stop();
    Result<ResourceSnapshot> read();

    void set_cpu(float percent);
    void set_memory(uint64_t available, uint64_t total);
    void set_throttled(bool throttled);
    void push_snapshot(ResourceSnapshot snap);   // Sequence mode
};
```

## Workload Model (`workload/dag.hpp`, `workload/generator.hpp`)

```cpp
class WorkloadDAG {
    void add_task(TaskId id, TaskProfile profile);
    void add_dependency(TaskId from, TaskId to);
    std::optional<TaskInfo> get_task(const TaskId& id) const;

    std::vector<TaskId> topological_order() const;
    bool has_cycle() const;
    size_t task_count() const;
    Duration total_compute_cost() const;
    Duration critical_path_cost() const;
    uint64_t peak_memory_estimate() const;

    std::vector<TaskId> ready_tasks() const;
    void mark_running(const TaskId& id);
    void mark_completed(const TaskId& id);
    void mark_failed(const TaskId& id);
};

struct WorkloadGenerator {
    static WorkloadDAG linear_chain(size_t length, TaskProfile profile);
    static WorkloadDAG fan_out_fan_in(size_t fan_width, TaskProfile profile);
    static WorkloadDAG diamond(size_t depth, size_t width, TaskProfile profile);
    static WorkloadDAG transformer_layers(size_t num_layers, uint64_t hidden_dim,
                                          uint64_t kv_cache_bytes_per_layer);
    static WorkloadDAG random_dag(size_t num_tasks, float edge_probability,
                                  TaskProfile min_profile, TaskProfile max_profile,
                                  std::mt19937& rng);
};
```

## Scheduler (`scheduler/`)

```cpp
struct SchedulingDecision {
    TaskId task_id;
    NodeId assigned_node;
    enum class Reason : uint8_t { LocalCapacity, LeastLoaded, ThresholdOffload,
                                  OptimizationResult, Fallback };
    Reason reason;
};

struct SchedulingPlan {
    std::vector<SchedulingDecision> decisions;
    Duration estimated_makespan{0};
};

class ISchedulingPolicy {
    virtual SchedulingPlan schedule(const WorkloadDAG& dag,
                                    const ResourceSnapshot& local,
                                    const ClusterView& cluster) = 0;
    virtual std::string_view name() const noexcept = 0;
};

class GreedyPolicy    : public ISchedulingPolicy { /* W_CPU·headroom + W_MEM·headroom - W_NET·cost */ };
class ThresholdPolicy : public ISchedulingPolicy { ThresholdPolicy(ThresholdPolicyConfig config); };
class OptimizerPolicy : public ISchedulingPolicy { OptimizerPolicy(OptimizerPolicyConfig config); };
```

## Executor (`executor/`)

```cpp
// Bounded lock-free MPMC ring (Vyukov); the ThreadPool's hand-off path
template <typename T>
class MpmcQueue {
    explicit MpmcQueue(size_t capacity);   // rounded up to power of two
    bool try_push(T&& value);              // false when full (backpressure)
    bool try_pop(T& out);                  // false when empty
    size_t size_approx() const;
    size_t capacity() const;
};

class ThreadPool {
    explicit ThreadPool(size_t num_threads = 0);   // 0 = hardware_concurrency

    template <std::invocable F>
    std::future<std::invoke_result_t<F>> submit(F&& func);

    template <std::invocable<std::stop_token> F>
    std::future<std::invoke_result_t<F, std::stop_token>> submit_cancellable(F&& func);

    size_t active_count() const noexcept;
    size_t queued_count() const noexcept;
    size_t thread_count() const noexcept;
};

class MemoryPool {
    explicit MemoryPool(size_t capacity_bytes);
    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t));  // nullptr on exhaustion
    void reset() noexcept;                  // wholesale arena reset
    bool can_allocate(size_t size) const noexcept;
    size_t capacity() const noexcept;
    size_t used() const noexcept;
};

class TaskRunner {
    struct ExecutionResult {
        TaskId task_id;
        TaskState final_state;
        Duration actual_duration;
        uint64_t peak_memory_bytes;
        std::optional<std::string> error_message;
    };
    ExecutionResult execute(const TaskId& id, const TaskProfile& profile,
                             MemoryPool& pool, std::stop_token stop);
};
```

## Network (`network/`)

```cpp
class PeerDiscovery {
    PeerDiscovery(NodeId node_id, uint16_t port,
                  uint32_t heartbeat_interval_ms, uint32_t peer_timeout_ms);
    void start();
    void stop();
    void on_peer_discovered(PeerCallback callback);
    void on_peer_lost(PeerCallback callback);
    void update_local_resources(const ResourceSnapshot& snapshot);
    size_t known_peer_count() const;
};

class TcpTransport {
    // Client
    Result<void> connect(const std::string& address, uint16_t port, uint32_t timeout_ms = 5000);
    Result<void> send(const std::vector<uint8_t>& data);
    Result<std::vector<uint8_t>> receive(uint32_t timeout_ms = 10000);
    void disconnect();
    // Server
    Result<void> listen(uint16_t port, int backlog = 16);
    void serve(MessageHandler handler);
    void stop_serving();
};

class AsyncTransport {   // coroutine + epoll server; same wire contract as TcpTransport
    explicit AsyncTransport(size_t handler_threads = 2, uint32_t io_timeout_ms = 10000);
    Result<void> listen(uint16_t port, int backlog = 16);
    void serve(MessageHandler handler);   // concurrent connections; handler runs off-reactor
    void stop_serving();
    size_t active_connections() const;
};

class ClusterViewManager {
    void update_peer(const NodeId& id, ResourceSnapshot snapshot);  // keeps known endpoint
    void update_peer(const PeerInfo& info);                          // full record incl. endpoint
    void mark_unreachable(const NodeId& id);
    void remove_peer(const NodeId& id);
    ClusterView snapshot() const;
    std::vector<NodeId> available_peers() const;
    std::optional<ResourceSnapshot> peer_resources(const NodeId& id) const;
    std::optional<PeerInfo> peer_info(const NodeId& id) const;       // node id -> endpoint
    size_t cluster_size() const;
};

struct PeerInfo {
    NodeId node_id;
    ResourceSnapshot resources;
    std::string address;    // learned from the advert sender's IP
    uint16_t tcp_port = 0;  // advertised offload port
    bool reachable = true;
};
```

## Orchestrator (`orchestrator/orchestrator.hpp`)

```cpp
struct OrchestrationResult {
    SchedulingPlan plan;
    size_t local_tasks, offloaded_tasks, completed_tasks, failed_tasks;
    size_t offload_fallbacks;   // offloads that failed over to local execution
    Duration total_duration;
};

// OffloadCodec (orchestrator/): Protobuf Envelope encode/decode + dispatch
struct OffloadCodec {
    enum class WireKind { Unknown, OffloadRequest, OffloadResponse,
                          WorkloadSubmission, WorkloadResult };
    static WireKind classify(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> encode_request(...);
    static bool decode_request(...);
    static std::vector<uint8_t> encode_response(...);
    static bool decode_response(...);
    static std::vector<uint8_t> encode_submission(const std::string& workload_id,
                                                   const WorkloadDAG& dag);
    static bool decode_submission(const std::vector<uint8_t>&, std::string&, WorkloadDAG&);
    static std::vector<uint8_t> encode_workload_result(...);
    static bool decode_workload_result(...);
};

template <typename MonitorT = LinuxMonitor>
class Orchestrator {
    struct Options {
        Config config;
        std::unique_ptr<ILogSink> log_sink;
        std::unique_ptr<ILogSink> metrics_sink;  // nullptr -> metrics discarded
        LogLevel log_level;
    };

    explicit Orchestrator(Options opts);
    Result<void> start();
    void stop();
    bool is_running() const noexcept;
    Result<OrchestrationResult> submit_workload(const WorkloadDAG& dag);

    // Accessors for testing
    MonitorT& monitor();
    ClusterViewManager& cluster();
};

// Usage:
Orchestrator<MockMonitor> orch(opts);    // Testing
Orchestrator<LinuxMonitor> orch(opts);   // Production
```

## Telemetry (`telemetry/`)

```cpp
class MetricsCollector {
    MetricsCollector(std::unique_ptr<IMetricsSink> sink);
    void record_resource_snapshot(const ResourceSnapshot& snap);
    void record_scheduling_decision(const SchedulingDecision& decision);
    void record_task_event(const TaskId& id, TaskState state, Duration duration);
    void record_peer_event(const NodeId& id, const std::string& event);
    void record_custom(const std::string& event_type, const std::string& json_payload);
};

class JsonFileSink : public IMetricsSink { /* NDJSON file output */ };
class NullSink     : public IMetricsSink { /* No-op for testing  */ };
class StdoutSink   : public ILogSink    { /* Console output      */ };
```

## Configuration (`core/config.hpp`)

```cpp
struct Config {
    struct { NodeId id; uint16_t port; } node;
    struct { std::string policy; ThresholdPolicyConfig threshold; OptimizerPolicyConfig optimizer; } scheduler;
    struct { uint32_t sampling_interval_ms; } monitor;
    struct { uint32_t heartbeat_interval_ms; uint32_t peer_timeout_ms;
             uint32_t offload_timeout_ms; } network;
    struct { uint32_t thread_count; uint32_t memory_pool_mb; } executor;
    struct { std::string log_dir; } telemetry;
};

Result<Config> load_config(const std::filesystem::path& path);
Config default_config();
```
