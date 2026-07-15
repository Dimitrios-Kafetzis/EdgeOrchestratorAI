# Research Context

## Academic Foundation

This project is a practical realization of research conducted at the **Athens University of Economics and Business** (AUEB), Department of Informatics, under the supervision of **Professor Iordanis Koutsopoulos**.

The EdgeOrchestrator system **adapts ideas from** published work on deploying AI inference workloads across resource-constrained wireless edge networks; it does not reimplement any paper's formulation exactly. The synthetic workload model mirrors the computational characteristics of transformer layers (configurable compute cost, memory footprint, KV-cache growth, inter-layer data dependencies). The optimizer policy is a fast heuristic in the same problem family as the published formulations — makespan minimization under per-node resource constraints with a dependency-aware timing model — with deliberate simplifications for a sub-millisecond scheduling budget: a scalar communication weight instead of per-link bandwidth, one execution slot per node, and hill-climbing local search instead of exact optimization. The honest relationship is "research-informed engineering," and the simplifications are documented in `docs/DESIGN.md` §7.3.

## Research Areas and Publications

### 1. DNN Inference Partitioning

Dynamic partitioning of deep neural network inference across heterogeneous edge devices with per-layer cost modeling and communication-aware placement. This work establishes the fundamental DAG-based workload model and resource-constrained optimization formulation that EdgeOrchestrator's scheduler policies are built upon.

**Mapping to code:** `workload/dag.hpp` (DAG model, critical path), `scheduler/optimizer_policy.cpp` (makespan minimization heuristic)

### 2. LLM Partitioning with Dynamic Memory Management

Extending DNN partitioning strategies to large language model (transformer) architectures, incorporating KV-cache-aware memory budgeting and dynamic re-partitioning during autoregressive inference. This research informs the transformer workload generator and memory-constrained scheduling decisions.

**Mapping to code:** `workload/generator.cpp` (transformer_layers topology with growing KV-cache), `scheduler/threshold_policy.cpp` (memory threshold awareness)

### 3. Multi-Layer Transformer Partitioning with Speculative Execution

Cross-layer similarity discovery and layer-level speculative execution for reduced end-to-end inference latency in multi-device deployments. This extends single-layer optimization to multi-layer settings with synergistic clustering — the active focus of the PhD dissertation.

**Mapping to code:** none yet — speculative execution and cross-layer similarity are **not implemented** in EdgeOrchestrator; they are listed on the v2.0 roadmap. What the optimizer shares with this line of work is only the multi-layer DAG framing and the dependency-aware makespan objective.

### 4. AI-based QoE Prediction for Teleoperated Autonomous Vehicles

Video quality prediction for autonomous vehicle teleoperation over wireless networks, using machine learning models to predict Mean Opinion Score from network metrics. Presented at IEEE MeditCom 2025.

**Mapping to code:** `workload/generator.cpp` (fan-out/fan-in topology models sensor fusion pipelines), `telemetry/` (structured event logging for QoE analysis)

### 5. Joint Optimization in Software-Defined Networks

Collaborative research on IDS-controller placement in SDN-MANETs (published at IEEE MILCOM), contributing to the network-aware scheduling components where placement decisions must account for communication topology and link capacity.

**Mapping to code:** `network/cluster_view.hpp` (topology-aware peer tracking), `scheduler/greedy_policy.cpp` (network transfer cost weighting)

## Detailed Research-to-Code Mapping

| Research Concept | Code Module | Implementation Detail |
|-----------------|-------------|----------------------|
| Per-layer cost modeling | `core/types.hpp` | `TaskProfile` with compute, memory, input/output bytes |
| DAG-based workload representation | `workload/dag.hpp` | `WorkloadDAG` with topological ordering and critical path |
| Transformer layer topology | `workload/generator.cpp` | `transformer_layers()` with attention + FFN per layer, growing KV-cache |
| Communication-aware partitioning | `scheduler/optimizer_policy.cpp` | Dependency-aware list-schedule makespan: cross-node edges pay `communication_weight × output_bytes`; a scalar weight, not a per-link bandwidth model |
| Resource-constrained placement | `scheduler/scheduler.hpp` | Per-node CPU and memory constraint enforcement |
| Dynamic resource sensing | `resource_monitor/linux_monitor.cpp` | Real-time /proc and /sys parsing with threshold callbacks |
| Adaptive offloading | `scheduler/threshold_policy.cpp` | Offload when CPU or memory exceeds configurable thresholds |
| Critical path scheduling | `scheduler/optimizer_policy.cpp` | Phase 1: DP-based critical path → assign to local node |
| Bin packing heuristic | `scheduler/optimizer_policy.cpp` | Phase 2: FFD assignment of remaining tasks |
| Local search improvement | `scheduler/optimizer_policy.cpp` | Phase 3: Random swap-based makespan reduction |
| Peer discovery | `network/peer_discovery.cpp` | UDP broadcast with packed 72-byte advertisement packets |
| Task offloading | `network/transport.cpp`, `network/async_transport.cpp`, `orchestrator/offload_codec.cpp` | Protobuf Envelope over length-prefixed TCP; async epoll server, sync client; local fallback on peer failure |
| Cluster management | `network/cluster_view.cpp` | Thread-safe peer state with heartbeat-based eviction |

## Experimental Validation

The project's synthetic workloads and resource monitoring are designed to support experimental validation on real Raspberry Pi 4B hardware running TinyLlama and GPT-2 models. The `tools/workload_injector.py` script and NDJSON telemetry enable systematic benchmarking of scheduling overhead, makespan, and resource utilization across different cluster sizes and workload types.

## Related EU Projects

This research was conducted in the context of several EU-funded projects:

- **TARGET-X** — Network automation and orchestration
- **SMOTANET** — Smart mobile and terrestrial autonomous networks
- **LEMONADE** — Low-energy mobile networks with advanced design
- **EREVOS** — Edge resource virtualization and orchestration

## Publication Venues

Research publications informing this project have appeared at:

- IEEE/IFIP WiOpt (Wireless Networks Optimization)
- ACM MobiHoc (Mobile Ad Hoc Networking and Computing)
- IEEE MILCOM (Military Communications)
- IEEE MeditCom (Mediterranean Communication)
- IEEE Vehicular Technology Magazine

## Author

**Dimitris Kafetzis**
PhD Candidate, Athens University of Economics and Business
Advisor: Professor Iordanis Koutsopoulos
Research Focus: Dynamic partitioning and resource orchestration for AI model deployment in resource-constrained wireless edge networks
Expected Defense: Summer 2026
