#!/usr/bin/env python3
"""
Workload Injector — Submit synthetic workloads to a running EdgeOrchestrator daemon.

Speaks the daemon's native wire format: a 4-byte big-endian length prefix
framing a serialized edge_orchestrator.Envelope (proto/protocol.proto).

Setup (once):
    pip install protobuf
    protoc --python_out=tools proto/protocol.proto

Usage:
    python3 tools/workload_injector.py --target localhost:5201 \
        --topology transformer --layers 12 --hidden-dim 768

Author: Dimitris Kafetzis
"""

import argparse
import socket
import struct
import sys

try:
    import protocol_pb2
except ImportError:
    sys.exit(
        "protocol_pb2 not found. Generate it with:\n"
        "    protoc --python_out=tools proto/protocol.proto\n"
        "and install the runtime with:\n"
        "    pip install protobuf"
    )


def create_linear_chain(num_tasks: int, compute_ms: float, memory_kb: int):
    """Build a linear-chain WorkloadSubmission."""
    submission = protocol_pb2.WorkloadSubmission()
    submission.workload_id = f"chain-{num_tasks}"
    for i in range(num_tasks):
        task = submission.tasks.add()
        task.task_id = f"chain_{i}"
        task.name = f"Chain Task {i}"
        task.profile.compute_cost_us = int(compute_ms * 1000)
        task.profile.memory_bytes = memory_kb * 1024
        task.profile.input_bytes = 1024
        task.profile.output_bytes = 1024
        if i > 0:
            task.dependencies.append(f"chain_{i-1}")
    return submission


def create_transformer(num_layers: int, hidden_dim: int):
    """Build a transformer-like WorkloadSubmission (attention + FFN per layer)."""
    submission = protocol_pb2.WorkloadSubmission()
    submission.workload_id = f"transformer-{num_layers}"
    for i in range(num_layers):
        attn = submission.tasks.add()
        attn.task_id = f"layer_{i}_attn"
        attn.name = f"Layer {i} Attention"
        attn.profile.compute_cost_us = hidden_dim * 2
        attn.profile.memory_bytes = hidden_dim * hidden_dim * 4
        attn.profile.input_bytes = hidden_dim * 4
        attn.profile.output_bytes = hidden_dim * 4
        if i > 0:
            attn.dependencies.append(f"layer_{i-1}_ffn")

        ffn = submission.tasks.add()
        ffn.task_id = f"layer_{i}_ffn"
        ffn.name = f"Layer {i} FFN"
        ffn.profile.compute_cost_us = hidden_dim * 4
        ffn.profile.memory_bytes = hidden_dim * hidden_dim * 4 * 4
        ffn.profile.input_bytes = hidden_dim * 4
        ffn.profile.output_bytes = hidden_dim * 4
        ffn.dependencies.append(attn.task_id)
    return submission


def recv_exact(sock: socket.socket, length: int) -> bytes:
    """Read exactly `length` bytes or raise ConnectionError."""
    buf = b""
    while len(buf) < length:
        chunk = sock.recv(length - len(buf))
        if not chunk:
            raise ConnectionError("Connection closed by daemon")
        buf += chunk
    return buf


def send_workload(target: str, submission, timeout_s: float) -> int:
    """Send a WorkloadSubmission and print the WorkloadResult. Returns exit code."""
    host, port_str = target.split(":")
    port = int(port_str)

    envelope = protocol_pb2.Envelope()
    envelope.workload_submission.CopyFrom(submission)
    payload = envelope.SerializeToString()

    print(f"Connecting to {host}:{port}...")
    print(f"Submitting '{submission.workload_id}' ({len(submission.tasks)} tasks, "
          f"{len(payload)} bytes)")

    with socket.create_connection((host, port), timeout=timeout_s) as sock:
        sock.sendall(struct.pack("!I", len(payload)) + payload)

        (resp_len,) = struct.unpack("!I", recv_exact(sock, 4))
        response = protocol_pb2.Envelope()
        response.ParseFromString(recv_exact(sock, resp_len))

    if not response.HasField("workload_result"):
        print("Unexpected response type from daemon", file=sys.stderr)
        return 1

    result = response.workload_result
    if not result.accepted:
        print(f"REJECTED: {result.error_message}", file=sys.stderr)
        return 1

    print(f"Workload '{result.workload_id}' complete:")
    print(f"  local tasks:       {result.local_tasks}")
    print(f"  offloaded tasks:   {result.offloaded_tasks}")
    print(f"  completed:         {result.completed_tasks}")
    print(f"  failed:            {result.failed_tasks}")
    print(f"  offload fallbacks: {result.offload_fallbacks}")
    print(f"  est. makespan:     {result.makespan_estimate_us} us")
    print(f"  wall time:         {result.total_duration_us} us")
    return 0 if result.failed_tasks == 0 else 1


def main():
    parser = argparse.ArgumentParser(description="EdgeOrchestrator Workload Injector")
    parser.add_argument("--target", default="localhost:5201", help="Daemon address (host:port)")
    parser.add_argument("--topology", choices=["chain", "transformer"], default="chain")
    parser.add_argument("--tasks", type=int, default=10, help="Number of tasks (chain)")
    parser.add_argument("--layers", type=int, default=12, help="Number of layers (transformer)")
    parser.add_argument("--hidden-dim", type=int, default=768, help="Hidden dimension (transformer)")
    parser.add_argument("--compute-ms", type=float, default=5.0, help="Compute cost per task (ms)")
    parser.add_argument("--memory-kb", type=int, default=64, help="Memory per task (KB)")
    parser.add_argument("--timeout", type=float, default=60.0, help="Socket timeout (seconds)")

    args = parser.parse_args()

    if args.topology == "chain":
        submission = create_linear_chain(args.tasks, args.compute_ms, args.memory_kb)
    else:
        submission = create_transformer(args.layers, args.hidden_dim)

    sys.exit(send_workload(args.target, submission, args.timeout))


if __name__ == "__main__":
    main()
