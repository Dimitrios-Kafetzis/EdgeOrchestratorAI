# EdgeOrchestrator

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/std/the-standard)
[![Tests](https://img.shields.io/badge/tests-160%20passing-brightgreen.svg)](#test-suite)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**A distributed, adaptive resource orchestrator for dynamic AI workload scheduling across embedded Linux edge nodes, written in modern C++20.**

EdgeOrchestrator is a lightweight daemon designed for clusters of Raspberry Pi (or similar ARM64 Linux) devices. It monitors hardware resources in real time, discovers peer nodes via UDP broadcast, and schedules computational workloads across the cluster using pluggable policies — adapting dynamically to changing resource availability. The project bridges academic research on DNN/LLM inference partitioning with a production-grade systems implementation.

## Architecture

```
                  ┌────────────────────────────────────────────────┐
                  │          EdgeOrchestrator Daemon (per node)     │
                  │                                                │
                  │   ┌─────────────────────────────────────────┐  │
                  │   │           Orchestrator Facade            │  │
                  │   │  Lifecycle · Workload Submission · TCP   │  │
                  │   └────────┬──────────┬──────────┬──────────┘  │
                  │            │          │          │              │
                  │   ┌────────▼───┐ ┌────▼────┐ ┌──▼─────────┐   │
                  │   │  Resource   │ │Scheduler│ │  Executor   │   │
                  │   │  Monitor    │ │ 3 modes │ │ThreadPool + │   │
                  │   │ /proc /sys  │ │         │ │ MemoryPool  │   │
                  │   └────────┬───┘ └────┬────┘ └──┬─────────┘   │
                  │            │          │          │              │
                  │   ┌────────▼───┐ ┌────▼────┐ ┌──▼─────────┐   │
                  │   │  Workload   │ │ Network │ │ Telemetry   │   │
                  │   │  DAG Model  │ │Discovery│ │   NDJSON    │   │
                  │   │ 5 topologies│ │TCP Xport│ │   events    │   │
                  │   └────────────┘ └─────────┘ └────────────┘   │
                  └────────────────────────────────────────────────┘
                            ▲                    ▲
                            │  UDP broadcast     │  TCP offload
                            ▼                    ▼
                  ┌──────────────┐     ┌──────────────┐
                  │  Peer Node B │     │  Peer Node C │
                  └──────────────┘     └──────────────┘
```

## Key Features

- **9-module architecture** — Core, Resource Monitor, Workload, Scheduler, Executor, Network, Telemetry, Orchestrator facade, Application
- **Three scheduling policies** — Greedy (weighted scoring), Threshold (adaptive offloading), Optimizer (critical path + bin packing + local search)
- **Real Linux monitoring** — `/proc/stat`, `/proc/meminfo`, `/sys/class/thermal`, `/proc/net/dev`, RPi throttle detection
- **UDP peer discovery** — 72-byte packed advertisement packets, configurable heartbeat and eviction timeout
- **TCP task offloading** — Length-prefixed binary framing, custom OffloadCodec, non-blocking I/O with `poll()`
- **Modern C++20** — Concepts, `std::jthread`, `std::atomic<shared_ptr>`, `Result<T,E>` monad, designated initializers, three-way comparison
- **160 tests** — Unit, integration, and end-to-end tests covering all modules
- **Cross-compilable** — CMake toolchain for `aarch64-linux-gnu`, CI via GitHub Actions

## Research Context

This project is a practical realization of research on **dynamic partitioning and resource orchestration for AI model deployment in resource-constrained wireless edge networks**, conducted at the Athens University of Economics and Business under Professor Iordanis Koutsopoulos. The optimizer scheduling policy implements lightweight versions of partitioning formulations from published work on DNN and transformer inference across heterogeneous edge devices.

See [docs/research_context.md](docs/research_context.md) for full academic context and [docs/DESIGN.md](docs/DESIGN.md) for the comprehensive design document.

## Quick Start

### Prerequisites

- GCC 12+ or Clang 15+ (C++20 required)
- CMake 3.22+
- Protobuf compiler and libraries (`apt install protobuf-compiler libprotobuf-dev`)

### Build and Test

```bash
git clone https://github.com/dimitris-kafetzis/EdgeOrchestrator.git
cd EdgeOrchestrator
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

### Run the Daemon

```bash
# Single node with demo workload
./build/edge_orchestrator --demo

# Production mode
./build/edge_orchestrator --config config/default.toml --node-id rpi-01 --port 5200

# Multi-node (separate terminals)
./build/edge_orchestrator --node-id node-A --port 5201
./build/edge_orchestrator --node-id node-B --port 5202
./build/edge_orchestrator --node-id node-C --port 5203
```

### Cross-Compile for Raspberry Pi

```bash
cmake -B build-arm \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm -j$(nproc)
```

## Project Structure

```
EdgeOrchestrator/                   90 files, ~8,400 lines of C++20
├── src/
│   ├── core/                       Types, Result<T,E>, Config, Logger, Concepts
│   │   ├── types.hpp               ResourceSnapshot, TaskProfile, Duration
│   │   ├── result.hpp              Monadic Result<T,E> with map/and_then
│   │   ├── concepts.hpp            ResourceMonitorLike, SchedulingPolicyLike
│   │   ├── config.{hpp,cpp}        TOML configuration loading
│   │   └── logger.{hpp,cpp}        Structured logging with pluggable sinks
│   ├── resource_monitor/           Real-time Linux resource sensing
│   │   ├── monitor.hpp             LinuxMonitor + MockMonitor declarations
│   │   ├── linux_monitor.cpp       /proc/stat, /proc/meminfo, thermal zones
│   │   └── mock_monitor.cpp        Static + sequence modes for testing
│   ├── workload/                   DAG-based workload modeling
│   │   ├── dag.{hpp,cpp}           WorkloadDAG: topo sort, critical path, cycle detection
│   │   └── generator.{hpp,cpp}     5 topologies: linear, fan-out/fan-in, diamond, transformer, random
│   ├── scheduler/                  Pluggable scheduling policies
│   │   ├── scheduler.hpp           SchedulingPlan, ISchedulingPolicy, ClusterView
│   │   ├── greedy_policy.{hpp,cpp} Weighted node scoring: CPU + memory - transfer cost
│   │   ├── threshold_policy.{hpp,cpp}  Adaptive offloading when load exceeds threshold
│   │   ├── optimizer_policy.{hpp,cpp}  Critical path → bin packing → local search
│   │   └── cluster_view.cpp        ClusterView helper methods
│   ├── executor/                   Managed task execution
│   │   ├── thread_pool.{hpp,cpp}   Fixed-size pool with std::jthread + stop tokens
│   │   ├── memory_pool.{hpp,cpp}   Arena allocator with budget enforcement
│   │   └── task_runner.{hpp,cpp}   Task execution with memory tracking
│   ├── network/                    Inter-node communication
│   │   ├── peer_discovery.{hpp,cpp}  UDP broadcast, heartbeat, peer eviction
│   │   ├── transport.{hpp,cpp}       Length-prefixed TCP with poll()-based I/O
│   │   └── cluster_view.{hpp,cpp}    Thread-safe peer state management
│   ├── orchestrator/               Top-level facade
│   │   ├── orchestrator.hpp        Orchestrator<MonitorT> template, lifecycle, wiring
│   │   └── offload_codec.cpp       Binary serialization for task offloading
│   ├── telemetry/                  Structured event logging
│   │   ├── metrics_collector.{hpp,cpp}  Event recording API
│   │   └── json_sink.{hpp,cpp}         NDJSON file output, null sink
│   └── app/
│       └── main.cpp                Daemon entry point, CLI, --demo mode
├── tests/
│   ├── unit/                       11 test files, 160 test cases
│   │   ├── test_result.cpp         Result monad operations
│   │   ├── test_types.cpp          ResourceSnapshot, TaskProfile
│   │   ├── test_config.cpp         TOML loading, defaults, error cases
│   │   ├── test_dag.cpp            DAG construction, topo sort, cycles, critical path
│   │   ├── test_generator.cpp      All 5 workload topologies
│   │   ├── test_thread_pool.cpp    Submit, concurrency, thread count
│   │   ├── test_memory_pool.cpp    Alloc, reset, capacity, OOM
│   │   ├── test_monitor.cpp        MockMonitor + LinuxMonitor
│   │   ├── test_scheduler.cpp      All 3 policies, cluster view, comparisons
│   │   ├── test_network.cpp        ClusterViewManager, TcpTransport, PeerDiscovery
│   │   └── test_orchestrator.cpp   OffloadCodec, Orchestrator<MockMonitor> E2E
│   ├── integration/
│   │   └── test_single_node.cpp    Full pipeline tests, two-node comms, E2E
│   └── benchmark/
│       └── bench_scheduler.cpp     Scheduling overhead measurement
├── docs/
│   ├── DESIGN.md                   Comprehensive design document (46KB)
│   ├── research_context.md         Academic context and publication mapping
│   └── API.md                      Module API quick reference
├── proto/
│   └── protocol.proto              Protobuf definitions (NodeAdvertisement, Offload)
├── config/
│   └── default.toml                Default daemon configuration
├── cmake/
│   └── toolchain-aarch64.cmake     ARM64 cross-compilation toolchain
├── tools/
│   ├── workload_injector.py        Submit workloads to running daemon
│   └── log_analyzer.py             NDJSON telemetry analysis
├── .github/workflows/ci.yml        GitHub Actions CI pipeline
├── .clang-format                   Code style configuration
└── LICENSE                         MIT License
```

## Test Suite

**160 tests, all passing.** Test time: ~9 seconds.

| Test Target | Tests | Coverage |
|-------------|-------|----------|
| `test_core` | 16 | Result monad, types, config parsing |
| `test_workload` | 27 | DAG operations, 5 workload generators |
| `test_executor` | 10 | Thread pool, memory pool |
| `test_monitor` | 13 | LinuxMonitor (/proc parsing), MockMonitor |
| `test_scheduler` | 23 | 3 policies, cluster view, cross-policy comparison |
| `test_network` | 26 | ClusterViewManager, TCP round-trip, UDP discovery + eviction |
| `test_orchestrator` | 15 | OffloadCodec, Orchestrator facade, policy selection |
| `test_integration` | 30 | Full pipeline, two-node TCP, E2E orchestration |

## C++20 Features Used

| Feature | Location | Purpose |
|---------|----------|---------|
| Concepts | `core/concepts.hpp` | `ResourceMonitorLike`, `SchedulingPolicyLike` — compile-time interface constraints |
| `std::jthread` | Monitor, Discovery, Thread Pool | Cooperative cancellation via `stop_token` |
| `std::atomic<shared_ptr>` | `LinuxMonitor` | Lock-free snapshot publication |
| Three-way comparison | `TaskProfile` | Priority ordering with `<=>` |
| Designated initializers | Throughout | Clean struct construction |
| `std::shared_mutex` | `ClusterViewManager` | Concurrent reader/writer access |
| `std::stop_source` / `std::stop_token` | Executor, Orchestrator | Graceful task cancellation |
| `constexpr` / `consteval` | Type utilities | Compile-time computation |

## Configuration

See [`config/default.toml`](config/default.toml) for all options. Key settings:

```toml
[node]
id = "rpi-01"
port = 5200

[scheduler]
policy = "optimizer"           # "greedy" | "threshold" | "optimizer"

[scheduler.threshold]
cpu_threshold_percent = 80.0
memory_threshold_percent = 85.0

[scheduler.optimizer]
max_iterations = 100
communication_weight = 0.3

[monitor]
sampling_interval_ms = 500

[network]
heartbeat_interval_ms = 1000
peer_timeout_ms = 5000

[executor]
thread_count = 0               # 0 = auto-detect hardware concurrency
memory_pool_mb = 64
```

## Author

**Dimitris Kafetzis**
- PhD Candidate, Athens University of Economics and Business
- Advisor: Professor Iordanis Koutsopoulos
- Research: Dynamic partitioning and resource orchestration for AI model deployment in resource-constrained wireless edge networks
- Director, IoT Department at DeepSea Technologies (Nabtesco subsidiary)

## License

This project is licensed under the MIT License — see [LICENSE](LICENSE) for details.
