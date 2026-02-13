#!/usr/bin/env python3
"""
Log Analyzer — Parse and visualize EdgeOrchestrator NDJSON telemetry logs.

Usage:
    python3 log_analyzer.py logs/node-01.ndjson --summary
    python3 log_analyzer.py logs/node-01.ndjson --event scheduling_decision

Author: Dimitris Kafetzis
"""

import argparse
import json
import sys
from collections import Counter
from pathlib import Path


def load_events(filepath: Path) -> list[dict]:
    """Load NDJSON events from a log file."""
    events = []
    with open(filepath) as f:
        for line_num, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError as e:
                print(f"Warning: skipping malformed line {line_num}: {e}", file=sys.stderr)
    return events


def print_summary(events: list[dict]) -> None:
    """Print a summary of the log file."""
    event_types = Counter(e.get("event", "unknown") for e in events)

    print(f"Total events: {len(events)}")
    print(f"\nEvent type breakdown:")
    for event_type, count in event_types.most_common():
        print(f"  {event_type}: {count}")

    # Task statistics
    task_events = [e for e in events if e.get("event") == "task_state_change"]
    if task_events:
        completed = [e for e in task_events if e.get("state") == "completed"]
        failed = [e for e in task_events if e.get("state") == "failed"]
        durations = [e.get("duration_us", 0) for e in completed]

        print(f"\nTask statistics:")
        print(f"  Completed: {len(completed)}")
        print(f"  Failed: {len(failed)}")
        if durations:
            print(f"  Avg duration: {sum(durations) / len(durations):.0f} μs")
            print(f"  Min duration: {min(durations)} μs")
            print(f"  Max duration: {max(durations)} μs")


def filter_events(events: list[dict], event_type: str) -> list[dict]:
    """Filter events by type."""
    return [e for e in events if e.get("event") == event_type]


def main():
    parser = argparse.ArgumentParser(description="EdgeOrchestrator Log Analyzer")
    parser.add_argument("logfile", type=Path, help="Path to NDJSON log file")
    parser.add_argument("--summary", action="store_true", help="Print summary statistics")
    parser.add_argument("--event", type=str, help="Filter by event type")
    parser.add_argument("--json", action="store_true", help="Output as JSON")

    args = parser.parse_args()

    if not args.logfile.exists():
        print(f"Error: {args.logfile} does not exist", file=sys.stderr)
        sys.exit(1)

    events = load_events(args.logfile)

    if args.summary:
        print_summary(events)
    elif args.event:
        filtered = filter_events(events, args.event)
        for e in filtered:
            if args.json:
                print(json.dumps(e))
            else:
                print(json.dumps(e, indent=2))
    else:
        print_summary(events)


if __name__ == "__main__":
    main()
