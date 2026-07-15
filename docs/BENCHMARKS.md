# Benchmarks

Measured results from real hardware: a Raspberry Pi 4 as the primary
edge target, and a two-node heterogeneous cluster (the Pi plus an x86
laptop) over a real wireless LAN for the cross-node experiments.

All numbers below were produced by the benchmark binaries in this
repository (`bench_scheduler`, `bench_offload`, `bench_queue`) and the
daemon itself. See [Reproducing](#reproducing) at the end.

---

## Testbed

**Edge node (`rpi1`)** — all single-node measurements ran here:

| | |
|---|---|
| Board | Raspberry Pi 4 Model B Rev 1.5 |
| CPU | 4 × Cortex-A72 @ 1.8 GHz, `performance` governor |
| RAM | 8 GB |
| OS | Debian 12 (bookworm), kernel 6.12.47+rpt-rpi-v8, aarch64 |
| Toolchain | GCC 12.2, CMake 3.25.1, protobuf 3.21.12, `-O2` Release build |
| Thermals | 52–58 °C during the campaign, `vcgencmd get_throttled` = `0x0` (never throttled) |

**Workstation (`devbox`)** — the second cluster node:

| | |
|---|---|
| CPU | AMD Ryzen AI 7 PRO 350 (16 hardware threads) |
| RAM | 30 GB |
| OS | Ubuntu 24.04, kernel 6.17 |
| Toolchain | GCC 13.3, CMake 3.28.3, protobuf 3.21.12 |

**Network** — both nodes on the same 192.168.0.0/24 WLAN through a
consumer access point (the Pi has no Ethernet uplink in this setup):

- Pi link: −63 dBm, 52 Mbit/s rx / 72 Mbit/s tx (802.11n rates)
- Idle ICMP RTT Pi→workstation: **3.4 ms avg / 5.8 ms max** while both
  radios are held awake
- With the workstation's Wi-Fi power management left in its default
  state, idle RTT degrades to **~100 ms avg with 560 ms spikes** — the
  access point buffers frames while the laptop's radio dozes. Cross-node
  measurements below were taken with a 20 pkt/s keepalive holding the
  radios awake; the dozing-radio numbers are reported where relevant
  because a real idle edge cluster would see them.

---

## Methodology

- **Scheduling latency / plan quality** (`bench_scheduler --sweep`, run
  on the Pi): for every cell of {topology} × {10, 50, 100, 500 tasks} ×
  {1, 3, 5 nodes} × {greedy, threshold, optimizer}, the policy's
  `schedule()` is timed over repeated iterations (300 for
  greedy/threshold, 100 for the optimizer, reduced to 100/30 at 500
  tasks) after 5 warmup calls; mean, stddev, and p99 are recorded.
  Cluster views are in-process resource snapshots (local node at 30 %
  CPU, peers at 20–60 %, 3 GiB free RAM each) — exactly the data
  structure the policies consume in production.
- **Plan quality** is the makespan of each policy's plan evaluated under
  one common dependency-aware list-schedule model (topological order,
  one execution slot per node, cross-node edges pay
  `output_bytes × 0.3 × 0.001 µs`). This is the same model the optimizer
  uses internally, applied uniformly so no policy grades its own
  homework. All three policies and the generators are deterministic
  (fixed seeds), so makespan is computed from one plan per cell.
- **Offload round trip** (`bench_offload`): 200 round trips per case
  over the real WLAN, with a fresh TCP connection per trip — exactly
  what `Orchestrator::offload_to_peer` does. Phases timed on the client:
  connect, Protobuf encode, wire (send + receive minus the
  server-reported execution time), execute (server-reported), Protobuf
  decode.
- **Monitor overhead**: whole-daemon CPU time (`/proc/<pid>/stat`) over
  60 s of idle running, at the default 500 ms sampling interval and at
  50 ms for comparison.
- Test suite: **178/178 tests pass on the Pi** (same suite as x86 CI).

---

## 1. Scheduling latency

Time for one `schedule()` call on the Pi (mean over iterations).
Random-DAG topology shown first — it is the least favourable realistic
case for the optimizer; the chain numbers below it are the cheapest.

**Random DAG (p = 0.1 edge density):**

| Tasks | Nodes | Greedy | Threshold | Optimizer |
|---|---|---|---|---|
| 10 | 1 | 7 µs | 7 µs | 27 µs |
| 10 | 3 | 8 µs | 7 µs | 683 µs |
| 10 | 5 | 8 µs | 7 µs | 780 µs |
| 50 | 1 | 55 µs | 55 µs | 328 µs |
| 50 | 3 | 57 µs | 56 µs | 7.1 ms |
| 50 | 5 | 58 µs | 56 µs | 8.7 ms |
| 100 | 1 | 157 µs | 157 µs | 1.0 ms |
| 100 | 3 | 159 µs | 157 µs | 23.7 ms |
| 100 | 5 | 161 µs | 159 µs | 27.2 ms |
| 500 | 1 | 3.1 ms | 3.1 ms | 30.2 ms |
| 500 | 3 | 3.2 ms | 3.1 ms | 669.9 ms |
| 500 | 5 | 3.2 ms | 3.2 ms | **929.9 ms** |

**Linear chain:**

| Tasks | Nodes | Greedy | Threshold | Optimizer |
|---|---|---|---|---|
| 10 | 1 | 9 µs | 8 µs | 31 µs |
| 10 | 5 | 9 µs | 8 µs | 1.0 ms |
| 50 | 5 | 43 µs | 40 µs | 4.8 ms |
| 100 | 5 | 86 µs | 83 µs | 10.0 ms |
| 500 | 5 | 462 µs | 452 µs | 60.3 ms |

Reading:

- Greedy and threshold are effectively free at any size (µs-scale,
  linear in tasks; ~indifferent to cluster size).
- The optimizer's cost is dominated by its local-search phase:
  `max_iterations × O(T + E)` makespan evaluations, each of which
  re-derives the topological order. With 1 node the local search
  short-circuits (nothing to swap), which is why the 1-node column is
  ~30× cheaper. Dense DAGs (random, 500 tasks: ~12 000 edges) push a
  single `schedule()` call to **0.9 s on the Pi** — far above any
  realistic scheduling budget.

## 2. Plan quality (makespan under the common model)

5-node cluster, evaluated per the methodology above. "Net" = makespan
saved vs greedy minus the extra scheduling latency the optimizer spent
to get it.

**Fan-out/fan-in (parallelizable):**

| Tasks | Greedy | Threshold | Optimizer | Opt vs Greedy | Opt sched cost | Net |
|---|---|---|---|---|---|---|
| 10 | 10.0 ms | 10.0 ms | 4.2 ms | −58% | +1.2 ms | **+4.6 ms** |
| 50 | 50.0 ms | 46.0 ms | 13.0 ms | −74% | +7.0 ms | **+30.0 ms** |
| 100 | 100.0 ms | 46.0 ms | 23.9 ms | −76% | +13.3 ms | **+62.8 ms** |
| 500 | 500.0 ms | 140.8 ms | 111.8 ms | −78% | +90.7 ms | **+297.5 ms** |

**Random DAG (mixed parallelism):**

| Tasks | Greedy | Threshold | Optimizer | Opt vs Greedy | Opt sched cost | Net |
|---|---|---|---|---|---|---|
| 10 | 14.3 ms | 14.3 ms | 3.6 ms | −75% | +0.8 ms | **+10.0 ms** |
| 50 | 69.2 ms | 58.9 ms | 16.3 ms | −77% | +8.6 ms | **+44.3 ms** |
| 100 | 131.8 ms | 80.6 ms | 34.2 ms | −74% | +27.1 ms | **+70.5 ms** |
| 500 | 635.5 ms | 215.1 ms | 187.5 ms | −70% | +926.7 ms | **−478.8 ms** |

**Transformer layers and linear chains (sequential):** all three
policies produce the critical-path makespan (identical numbers, 0 %
improvement); the optimizer keeps 100 % of tasks local at every size —
it correctly refuses to distribute a dependency chain — so its only
effect is wasted scheduling latency (−1 to −64 ms net).

Where each task ends up (optimizer, 5 nodes, fraction kept local):
chain/transformer **100 %** at all sizes; fan-out 20–30 %;
random ~20 %. Greedy with peers present sends **everything to a single
peer** (its headroom score never updates CPU during planning), which is
why its makespan never improves — a genuine weakness the sweep exposed.

**The crossover, stated plainly:** on parallelizable DAGs at 5 nodes the
optimizer improves makespan by 58–78 % over greedy, and the improvement
pays for its own scheduling latency from 10 tasks up to roughly 100–500
tasks. Beyond that its latency grows superlinearly with DAG density
(random-500: 0.93 s to save 0.45 s — a net loss; threshold gets 80 % of
the benefit for 0.003 % of the scheduling cost). On sequential
workloads (transformer inference — the primary motivating workload) the
optimizer never pays for itself; it only proves that distribution is
pointless. That is why the deployed default is threshold, with the
optimizer as an opt-in for known-parallel DAGs below a few hundred
tasks.

## 3. Offload round-trip latency (real WLAN)

`bench_offload`, 200 round trips per case, fresh connection per trip
(as in production), radios held awake. Mean per phase, µs:

**Pi → workstation** (the realistic direction: constrained node
offloads to a stronger peer):

| Case | Connect | Serialize | Wire | Execute | Deserialize | Total | p99 |
|---|---|---|---|---|---|---|---|
| empty task, 0 B | 2 670 | 3 | 2 824 | 0 | 2 | **5.5 ms** | 14.7 ms |
| empty task, 64 KiB payload | 7 403 | 135 | 30 750 | 0 | 8 | **38.3 ms** | 58.3 ms |
| 1 ms task, 0 B | 3 029 | 4 | 3 202 | 1 000 | 3 | **7.2 ms** | 26.4 ms |
| 10 ms task, 0 B | 2 694 | 12 | 2 968 | 10 000 | 4 | **15.7 ms** | 23.9 ms |
| 10 ms task, 64 KiB | 6 785 | 136 | 30 776 | 10 000 | 23 | **47.7 ms** | 72.4 ms |

**Workstation → Pi** is symmetric within noise (5.3 ms empty; 54–58 ms
with 64 KiB — the Pi's lower rx bitrate shows on payload uploads).

Where the time goes:

- **Protobuf is irrelevant**: encode ≤ 136 µs even with a 64 KiB
  payload, decode ≤ 23 µs. The Envelope codec is nowhere near the
  bottleneck.
- **The network is everything**: an empty offload costs ~5.5 ms, split
  roughly half TCP connect, half request/response flight time — this is
  the WLAN's air-time cost, not software. A 64 KiB payload adds ~30 ms
  (~2 MB/s effective, consistent with a fresh-connection TCP over a
  52–72 Mbit/s link).
- **Break-even**: with a ~5.5 ms fixed floor, offloading a task is only
  worth it when the compute saved exceeds that. On this link, tasks
  under ~10 ms of compute (with small payloads) are marginal; tasks
  carrying 64 KiB need to save ~40 ms to break even. 0 failures in
  2 000 round trips.
- With dozing radios (no keepalive), the p99 climbs to **~570 ms** —
  the first offload after an idle period pays the AP's power-save
  buffering. A production deployment on Wi-Fi should keep heartbeat
  traffic flowing (ours does, every 2 s) or pin power management off.

## 4. Resource-monitor overhead

Whole idle daemon on the Pi (includes discovery heartbeats, eviction
scans, epoll server, telemetry — not just the monitor):

| Sampling interval | Idle daemon CPU (one core) |
|---|---|
| 500 ms (default) | **0.23 %** |
| 50 ms (10×) | 0.60 % |

The 9× extra samples cost 0.37 pp, i.e. **~0.2 ms of CPU per `/proc`
sample**; at the default rate the monitor itself is ~0.04 % CPU and the
remaining ~0.19 % is the rest of the daemon. The 500 ms interval is
comfortably justified — and even 50 ms would be, if a scheduler needed
fresher signals.

## 5. Discovery stability under packet loss

Two-node cluster (Pi + workstation daemons over the WLAN), 2 s
heartbeat / 6 s eviction timeout. Symmetric random drop applied to the
discovery datagrams (UDP 5201 — at the benchmarked commit discovery
shared the TCP port number; it has since moved to its own
`discovery_port`, 5200 by default) at the Pi via
`iptables -m statistic`; 3 min clean baseline, then 5 min per loss
level. "Spurious eviction" = a `Peer lost` on either daemon while both
were in fact alive.

| Loss | Spurious evictions | Recovery |
|---|---|---|
| 0 % (baseline, 3 min) | 0 | — |
| 10 % (5 min) | **1** | re-discovered 1 s later |
| 30 % (5 min) | **3** (2 on the workstation, 1 on the Pi) | re-discovered 1–2 s later |

This matches the math: an eviction needs > 6 s of silence, i.e. ~3
consecutive heartbeats lost — probability 0.1 % per eviction-check
window at 10 % loss, 2.7 % at 30 %; over ~100 windows that predicts ~0
and ~3 events respectively, which is what happened. Two properties
kept the view stable rather than flapping:

- Recovery is one heartbeat: a single datagram getting through re-adds
  the peer, so each eviction healed at the next 2 s beat.
- Discovery and offloading are decoupled: a briefly-evicted peer only
  shrinks the *cluster view*; in-flight offloads run on their own TCP
  connection and are unaffected.

So at DDIL-like 30 % loss the cluster view is wrong for a couple of
seconds roughly once every 100 s. If that mattered for a deployment,
the obvious fix is hysteresis (require k missed *windows*, or evict at
timeout but re-admit only after 2 consecutive heartbeats) — but the
measured 2 s/6 s defaults did not flap badly enough to justify the
added staleness.

## 6. Partition behaviour

Same two-node cluster. Partition = iptables DROP at the Pi for the
workstation's discovery datagrams and offload TCP (port 5201), i.e. a
full application-level split while the LAN itself stays up. The
workstation daemon ran a threshold policy forced to offload; 8-task
chains (20 ms/task) were injected at each stage via
`tools/workload_injector.py`.

Timeline (from both daemons' NDJSON logs):

| t | Event |
|---|---|
| 0 s | partition applied |
| +5 s | Pi logs `Peer lost: devbox` (last heartbeat + 6 s timeout) |
| +8 s | workstation logs `Peer lost: rpi1` |
| +34 s | partition removed |
| +36 s | **both** daemons log `Peer discovered` again (next heartbeat) |

Workload outcomes at each stage:

| Stage | Offloaded | Fell back | Completed | Wall time |
|---|---|---|---|---|
| A — healthy | 8/8 | 0 | 8/8 | 91 ms |
| B — injected 4 s into partition (peer still in view) | 0 (8 attempted) | **8** | 8/8 | **10.03 s** |
| C — injected after eviction | 0 | 0 (scheduled local directly) | 8/8 | 20 ms |
| D — after heal | 8/8 | 0 | 8/8 | 91 ms |

What actually happens, in words: **no tasks are lost, no split-brain,
no duplicate execution** — each side simply degrades to a single-node
orchestrator and heals within one heartbeat of connectivity returning.
The one genuine wart is stage B: while the dead peer is still in the
cluster view (the 6 s eviction window), every offload attempt hangs for
the full 10 s `offload_timeout_ms` connect deadline before falling back
local — a 500× slowdown vs stage C, where the same workload just runs
locally in 20 ms. The connect deadline should be 1–2 s (well under the
eviction timeout), which would cap the worst-case stall at the
partition moment to ~2 s. That is the top actionable finding of this
whole campaign.

## 7. Executor queue on 4 cores

`bench_queue` — the ThreadPool's lock-free MPMC hand-off vs the
mutex+`std::queue` it replaced, on the Pi's 4 Cortex-A72 cores
(Mops/s, 250 k ops/producer):

| Producers × Consumers | mutex+queue | lock-free | Speedup |
|---|---|---|---|
| 1 × 1 | 3.14 | 13.32 | **4.2×** |
| 2 × 2 | 2.78 | 7.57 | **2.7×** |
| 4 × 4 | 2.68 | 4.86 | **1.8×** |
| 8 × 8 | 2.59 | 3.66 | **1.4×** |

A 500 k-ops re-run reproduces the ordering (2.4×/3.1×/1.9×/1.3×).
Notably this is the opposite profile from the 16-core x86 dev machine,
where the lock-free queue *lost* at 1×1 (~0.9×) and won ~1.3× at
2×2/4×4: on the A72, taking a contended mutex is comparatively much
more expensive than a CAS, so the lock-free queue wins everywhere —
including the uncontended case. The hand-off is worth ~0.3 µs/op either
way; it matters for micro-tasks, not for the ms-scale tasks in the
sweep above.

---

## Limitations

Stated up front because they bound what these numbers can claim:

1. **One physical Pi.** Every "3-node" or "5-node" scheduling figure
   uses simulated in-process cluster views (identical 3 GiB-free peers).
   The policies consume exactly this data structure in production, so
   the *latency* numbers are real, but no multi-node makespan was
   *executed* — plan quality is a model quantity. The only physically
   distributed measurements are §3, §5, §6 on the two-node
   Pi + workstation cluster.
2. **Makespan model, not wall clock.** Plan quality uses the same
   dependency-aware list-schedule model the optimizer optimizes
   (uniform across policies, but it shares that model's assumptions:
   one execution slot per node, linear transfer cost, perfect cost
   estimates). It also means the optimizer is evaluated by a judge that
   thinks like it; greedy/threshold could look relatively better under
   a different network model.
3. **Synthetic tasks.** `TaskRunner` burns calibrated CPU time; no real
   model inference, no memory-bandwidth contention. Offload requests
   carry task descriptors (plus an optional payload in `bench_offload`),
   not real tensors.
4. **Wi-Fi only, one topology.** Both nodes sit on one consumer AP
   (two air hops per packet). Ethernet would be faster and steadier; a
   congested or larger RF environment would be worse. The laptop's
   power-save artifact is documented in §3.
5. **Single campaign run.** Within-run distributions (stddev/p99) are
   reported, but cell-to-cell reproducibility across days/temperatures
   was not characterized. No thermal throttling occurred (verified).
6. **Loss/partition injected at one end.** iptables on the Pi drops
   both directions' discovery datagrams, but drop decisions are made at
   the Pi, not on the air path; real RF loss is bursty, not Bernoulli.

## Reproducing

```bash
# on the target machine
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build -j$(nproc)

./build/tests/bench_scheduler --sweep > sweep.csv   # §1–2 (≈15 min on a Pi 4)
./build/tests/bench_queue                           # §7

# §3: on the server node, then on the client node
./build/tests/bench_offload --serve 20117
./build/tests/bench_offload --client <server-ip> 20117 200
```

Discovery/partition experiments (§5–6) run two daemons
(`edge_orchestrator --node-id A` / `--node-id B` on two machines,
same `discovery_port`) with loss injected via
`iptables -I INPUT/OUTPUT -p udp --dport 5200 -m statistic --mode random
--probability 0.3 -j DROP`, and workloads injected via
`tools/workload_injector.py`.
