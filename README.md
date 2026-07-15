# EdgeOrchestrator

[![CI](https://github.com/Dimitrios-Kafetzis/EdgeOrchestratorAI/actions/workflows/ci.yml/badge.svg)](https://github.com/Dimitrios-Kafetzis/EdgeOrchestratorAI/actions/workflows/ci.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/std/the-standard)
[![Tests](https://img.shields.io/badge/tests-178%20passing-brightgreen.svg)](#test-suite)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**A distributed, adaptive resource orchestrator for dynamic AI workload scheduling across embedded Linux edge nodes, written in modern C++20.**

EdgeOrchestrator is a lightweight daemon designed for clusters of Raspberry Pi (or similar ARM64 Linux) devices. It monitors hardware resources in real time, discovers peer nodes via UDP broadcast, and schedules computational workloads across the cluster using pluggable policies — adapting dynamically to changing resource availability. The project bridges academic research on DNN/LLM inference partitioning with a production-grade systems implementation.

## Architecture

```
                  ┌────────────────────────────────────────────────┐
                  │          EdgeOrchestrator Daemon (per node)    │
                  │                                                │
                  │   ┌─────────────────────────────────────────┐  │
                  │   │           Orchestrator Facade           │  │
                  │   │  Lifecycle · Workload Submission · TCP  │  │
                  │   └────────┬──────────┬──────────┬──────────┘  │
                  │            │          │          │             │
                  │   ┌────────▼───┐ ┌────▼────┐ ┌───▼─────────┐   │
                  │   │  Resource  │ │Scheduler│ │  Executor   │   │
                  │   │  Monitor   │ │ 3 modes │ │ThreadPool + │   │
                  │   │ /proc /sys │ │         │ │ MemoryPool  │   │
                  │   └────────┬───┘ └────┬────┘ └───┬─────────┘   │
                  │            │          │          │             │
                  │   ┌────────▼────┐ ┌────▼────┐ ┌───▼─────────┐  │
                  │   │  Workload   │ │ Network │ │ Telemetry   │  │
                  │   │  DAG Model  │ │Discovery│ │   NDJSON    │  │
                  │   │ 5 topologies│ │TCP Xport│ │   events    │  │
                  │   └─────────────┘ └─────────┘ └─────────────┘  │
                  └────────────────────────────────────────────────┘
                            ▲                    ▲
                            │  UDP broadcast     │  TCP offload
                            ▼                    ▼
                  ┌──────────────┐            ┌──────────────┐
                  │  Peer Node B │            │  Peer Node C │
                  └──────────────┘            └──────────────┘
```

## Key Features

- **9-module architecture** — Core, Resource Monitor, Workload, Scheduler, Executor, Network, Telemetry, Orchestrator facade, Application
- **Three scheduling policies** — Greedy (weighted scoring), Threshold (adaptive offloading), Optimizer (critical path + bin packing + local search)
- **Real Linux monitoring** — `/proc/stat`, `/proc/meminfo`, `/sys/class/thermal`, `/proc/net/dev`, RPi throttle detection
- **UDP peer discovery** — 72-byte packed advertisement packets (endian-safe, versioned), configurable heartbeat and eviction timeout
- **TCP task offloading** — Protobuf `Envelope` protocol over length-prefixed framing; coroutine + `epoll` async server, `poll()`-based synchronous client; peer death fails over to local execution
- **Workload submission over the wire** — clients (incl. the Python injector) submit whole DAGs via the same Protobuf schema and get executed results back
- **Lock-free executor hand-off** — bounded Vyukov MPMC queue with semaphore-gated workers, benchmarked against the mutex baseline
- **Modern C++20** — Concepts (enforced by static_assert), coroutines, `std::jthread`, `std::counting_semaphore`, `std::atomic<shared_ptr>`, `Result<T,E>` monad
- **178 tests** — Unit, integration, and end-to-end tests covering all modules, including live two-node offload and failure-path coverage
- **Measured, not claimed** — full benchmark campaign on real Raspberry Pi 4 hardware and a two-node Pi + x86 cluster over a live WLAN: [docs/BENCHMARKS.md](docs/BENCHMARKS.md)
- **Cross-compilable** — CMake toolchain for `aarch64-linux-gnu`, CI via GitHub Actions; builds natively on Raspberry Pi OS bookworm (GCC 12)

## Documentation Map

Start here depending on what you are after:

| Document | Read it when you want… |
|---|---|
| this README | to build, run, and get a first mental model of the system |
| [docs/DESIGN.md](docs/DESIGN.md) | the comprehensive design document: module responsibilities, threading model, wire protocols, and the rationale (and rejected alternatives) behind each decision |
| [docs/API.md](docs/API.md) | a quick per-module API reference for writing code against the library targets |
| [docs/BENCHMARKS.md](docs/BENCHMARKS.md) | measured numbers from real hardware: scheduling latency and plan quality per policy, offload round-trip breakdown, monitor overhead, discovery under packet loss, partition behaviour — plus methodology and limitations |
| [docs/research_context.md](docs/research_context.md) | the academic lineage: which published formulations the optimizer policy adapts, and what was simplified on the way to a running system |
| [config/default.toml](config/default.toml) | every runtime knob, commented |
| `tools/` | the Python workload injector and NDJSON log analyzer (usage below) |

The source itself is documented with Doxygen-style comments (`@brief`,
`@param`, design-rationale blocks in the file headers), so an API site
can be generated with any Doxygen-compatible pipeline (e.g. mkdocs +
mkdoxy) without further annotation work.

## Research Context

This project is a practical realization of research on **dynamic partitioning and resource orchestration for AI model deployment in resource-constrained wireless edge networks**, conducted at the Athens University of Economics and Business under Professor Iordanis Koutsopoulos. The optimizer scheduling policy adapts the partitioning formulations from published work on DNN and transformer inference into a fast, dependency-aware heuristic — the simplifications and their rationale are documented in [docs/DESIGN.md](docs/DESIGN.md) §7.3.

See [docs/research_context.md](docs/research_context.md) for full academic context and [docs/DESIGN.md](docs/DESIGN.md) for the comprehensive design document.

## Quick Start

### Prerequisites

- GCC 12+ or Clang 15+ (C++20 required)
- CMake 3.22+
- Protobuf compiler and libraries (`apt install protobuf-compiler libprotobuf-dev`)

### Build and Test

```bash
git clone https://github.com/Dimitrios-Kafetzis/EdgeOrchestratorAI.git
cd EdgeOrchestratorAI
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
```

### Run the Daemon

```bash
# Single node with demo workload (schedules a transformer DAG under all
# three policies, prints the comparison, executes the optimizer's plan)
./build/edge_orchestrator --demo

# Production mode
./build/edge_orchestrator --config config/default.toml --node-id rpi-01 --port 5201

# Multi-node on one machine (separate terminals): distinct TCP ports,
# shared UDP discovery_port (5200 from default.toml) — the daemons
# discover each other via broadcast within one heartbeat (2 s)
./build/edge_orchestrator --node-id node-A --port 5201
./build/edge_orchestrator --node-id node-B --port 5202
./build/edge_orchestrator --node-id node-C --port 5203

# Multi-node across machines: identical invocation on each host, same
# config; only --node-id needs to differ
./build/edge_orchestrator --node-id rpi-01     # on the Pi
./build/edge_orchestrator --node-id devbox     # on the workstation
```

### Submit a Workload

`tools/workload_injector.py` is a standalone Protobuf client that sends a
whole DAG to a running daemon over TCP and waits for the executed result
(local/offloaded/failed counts, makespan, wall time):

```bash
# One-time: generate the Python bindings and install the runtime
protoc -Iproto --python_out=tools protocol.proto
pip install "protobuf>=4.21"

python3 tools/workload_injector.py --target 127.0.0.1:5201 \
    --topology chain --tasks 8 --compute-ms 20

# Then inspect the daemon's NDJSON telemetry
python3 tools/log_analyzer.py --summary logs/metrics.ndjson
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
EdgeOrchestrator/                   ~10,700 lines of C++20 across 79 tracked files
├── src/
│   ├── core/                       Types, Result<T,E>, Config, Logger, Concepts
│   │   ├── types.hpp               ResourceSnapshot, TaskProfile, Duration
│   │   ├── result.hpp              Monadic Result<T,E> with map/and_then
│   │   ├── concepts.hpp            ResourceMonitorLike, SchedulingPolicyLike
│   │   ├── async.hpp               Coroutine primitives: Async<T>, DetachedTask
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
│   │   ├── mpmc_queue.hpp          Bounded lock-free Vyukov MPMC queue (the pool's hand-off)
│   │   ├── memory_pool.{hpp,cpp}   Arena allocator with budget enforcement
│   │   └── task_runner.{hpp,cpp}   Task execution with memory tracking
│   ├── network/                    Inter-node communication
│   │   ├── peer_discovery.{hpp,cpp}  UDP broadcast, heartbeat, peer eviction
│   │   ├── transport.{hpp,cpp}       Length-prefixed TCP with poll()-based I/O (sync client)
│   │   ├── reactor.{hpp,cpp}         epoll reactor driving the async server
│   │   ├── async_transport.{hpp,cpp} Coroutine TCP server (many connections, one reactor thread)
│   │   └── cluster_view.{hpp,cpp}    Thread-safe peer state management
│   ├── orchestrator/               Top-level facade
│   │   ├── orchestrator.hpp        Orchestrator<MonitorT> template, lifecycle, wiring
│   │   └── offload_codec.cpp       Protobuf Envelope encode/decode (quarantined here)
│   ├── telemetry/                  Structured event logging
│   │   ├── metrics_collector.{hpp,cpp}  Event recording API
│   │   └── json_sink.{hpp,cpp}         NDJSON file output, null sink
│   └── app/
│       └── main.cpp                Daemon entry point, CLI, --demo mode
├── tests/
│   ├── unit/                       11 test files (see table below)
│   ├── integration/
│   │   └── test_single_node.cpp    Full pipeline tests, two-node comms, E2E
│   └── benchmark/
│       ├── bench_scheduler.cpp     Scheduling latency + --sweep policy campaign (CSV)
│       ├── bench_queue.cpp         Lock-free MPMC vs mutex+queue hand-off throughput
│       └── bench_offload.cpp       Cross-node offload round trip, per-phase timing
├── docs/
│   ├── DESIGN.md                   Comprehensive design document
│   ├── API.md                      Module API quick reference
│   ├── BENCHMARKS.md               Measured results from Raspberry Pi hardware
│   └── research_context.md         Academic context and publication mapping
├── proto/
│   └── protocol.proto              Protobuf definitions (Envelope, Offload, Workload)
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

**178 tests, all passing** — on x86 CI (Debug, Release, ASan/UBSan) and
natively on a Raspberry Pi 4. Test time: ~9 seconds.

| Test Target | Tests | Coverage |
|-------------|-------|----------|
| `test_core` | 16 | Result monad, types, config parsing |
| `test_workload` | 49 | DAG operations, 5 workload generators |
| `test_executor` | 15 | Thread pool, MPMC queue, memory pool |
| `test_monitor` | 13 | LinuxMonitor (/proc parsing), MockMonitor |
| `test_scheduler` | 22 | 3 policies, cluster view, cross-policy comparison |
| `test_network` | 30 | ClusterViewManager, TCP round-trip, UDP discovery + eviction |
| `test_orchestrator` | 22 | OffloadCodec, Orchestrator facade, policy selection |
| `test_integration` | 11 | Full pipeline, two-node TCP, E2E orchestration |

## Benchmarks

Full campaign with methodology and limitations in
[docs/BENCHMARKS.md](docs/BENCHMARKS.md); measured on a Raspberry Pi 4
(8 GB) and a two-node Pi + x86 cluster over a live WLAN. Headlines:

- Greedy and threshold schedule in **microseconds** at every size; the
  optimizer improves makespan on parallelizable DAGs by **58–78 %** at 5
  nodes but its latency grows to ~0.9 s at 500 dense tasks — the
  crossover analysis is why threshold is the deployed default.
- On sequential DAGs (transformer inference) the optimizer correctly
  keeps 100 % of tasks local: distribution cannot beat the critical path.
- A real offload round trip costs **5.5 ms** (empty) / 38 ms (+64 KiB)
  Pi↔x86 over Wi-Fi; Protobuf encode/decode is ≤ 136 µs of that.
- The idle daemon costs **0.23 %** of one Pi core at the default 500 ms
  sampling; discovery survives 30 % datagram loss with ~3 short-lived
  spurious evictions per 5 min; a network partition loses **zero tasks**.

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
id = "node-01"
port = 5201                    # TCP: offload + workload submission
discovery_port = 5200          # UDP: heartbeat broadcast (cluster-wide)

[scheduler]
policy = "greedy"              # "greedy" | "threshold" | "optimizer"

[scheduler.threshold]
cpu_threshold_percent = 75.0
memory_threshold_percent = 80.0

[scheduler.optimizer]
max_iterations = 100
communication_weight = 0.3

[monitor]
sampling_interval_ms = 500

[network]
heartbeat_interval_ms = 2000
peer_timeout_ms = 6000

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
