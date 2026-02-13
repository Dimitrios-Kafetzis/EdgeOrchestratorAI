#!/usr/bin/env python3
"""
Workload Injector — Submit synthetic workloads to a running EdgeOrchestrator daemon.

Usage:
    python3 workload_injector.py --target localhost:5201 \
        --topology transformer --layers 12 --hidden-dim 768

Author: Dimitris Kafetzis
"""

import argparse
import json
import socket
import struct
import sys


def create_linear_chain(num_tasks: int, compute_ms: float, memory_kb: int) -> dict:
    """Generate a linear chain workload descriptor."""
    tasks = []
    for i in range(num_tasks):
        tasks.append({
            "id": f"chain_{i}",
            "name": f"Chain Task {i}",
            "profile": {
                "compute_cost_us": int(compute_ms * 1000),
                "memory_bytes": memory_kb * 1024,
                "input_bytes": 1024,
                "output_bytes": 1024,
            },
            "dependencies": [f"chain_{i-1}"] if i > 0 else [],
        })
    return {"topology": "linear_chain", "tasks": tasks}


def create_transformer(num_layers: int, hidden_dim: int) -> dict:
    """Generate a transformer-like workload descriptor."""
    tasks = []
    for i in range(num_layers):
        # Each layer has attention + FFN sub-tasks
        attn_id = f"layer_{i}_attn"
        ffn_id = f"layer_{i}_ffn"

        tasks.append({
            "id": attn_id,
            "name": f"Layer {i} Attention",
            "profile": {
                "compute_cost_us": hidden_dim * 2,
                "memory_bytes": hidden_dim * hidden_dim * 4,
                "input_bytes": hidden_dim * 4,
                "output_bytes": hidden_dim * 4,
            },
            "dependencies": [f"layer_{i-1}_ffn"] if i > 0 else [],
        })
        tasks.append({
            "id": ffn_id,
            "name": f"Layer {i} FFN",
            "profile": {
                "compute_cost_us": hidden_dim * 4,
                "memory_bytes": hidden_dim * hidden_dim * 4 * 4,
                "input_bytes": hidden_dim * 4,
                "output_bytes": hidden_dim * 4,
            },
            "dependencies": [attn_id],
        })
    return {"topology": "transformer", "tasks": tasks}


def send_workload(target: str, workload: dict) -> None:
    """Send a workload to the daemon via TCP."""
    host, port_str = target.split(":")
    port = int(port_str)

    payload = json.dumps(workload).encode("utf-8")
    header = struct.pack("!I", len(payload))

    print(f"Connecting to {host}:{port}...")
    print(f"Sending workload: {workload['topology']} ({len(workload['tasks'])} tasks)")

    # TODO: Actually connect and send when daemon supports it
    print(f"[DRY RUN] Would send {len(payload)} bytes to {target}")
    print(json.dumps(workload, indent=2))


def main():
    parser = argparse.ArgumentParser(description="EdgeOrchestrator Workload Injector")
    parser.add_argument("--target", default="localhost:5201", help="Daemon address (host:port)")
    parser.add_argument("--topology", choices=["chain", "transformer"], default="chain")
    parser.add_argument("--tasks", type=int, default=10, help="Number of tasks (chain)")
    parser.add_argument("--layers", type=int, default=12, help="Number of layers (transformer)")
    parser.add_argument("--hidden-dim", type=int, default=768, help="Hidden dimension (transformer)")
    parser.add_argument("--compute-ms", type=float, default=5.0, help="Compute cost per task (ms)")
    parser.add_argument("--memory-kb", type=int, default=64, help="Memory per task (KB)")

    args = parser.parse_args()

    if args.topology == "chain":
        workload = create_linear_chain(args.tasks, args.compute_ms, args.memory_kb)
    else:
        workload = create_transformer(args.layers, args.hidden_dim)

    send_workload(args.target, workload)


if __name__ == "__main__":
    main()
