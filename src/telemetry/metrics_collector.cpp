/**
 * @file metrics_collector.cpp
 * @brief MetricsCollector implementation.
 * @author Dimitris Kafetzis
 *
 * Events are hand-assembled NDJSON lines, one object per line, pushed
 * through the same ILogSink abstraction the logger uses. Hand-assembled
 * because the schema is tiny, fixed, and hot (a task event per task per
 * workload): pulling in a JSON library for five string formats would be
 * a dependency in exchange for nothing. The one rule callers must keep
 * is that record_custom's payload is already-valid JSON — it is spliced
 * in verbatim. tools/log_analyzer.py is the consumer these lines are
 * shaped for.
 */

#include "telemetry/metrics_collector.hpp"

#include <chrono>
#include <sstream>

namespace edge_orchestrator {

MetricsCollector::MetricsCollector(std::unique_ptr<ILogSink> sink)
    : sink_(std::move(sink)) {}

void MetricsCollector::record_resource_snapshot(const ResourceSnapshot& snap) {
    std::ostringstream oss;
    oss << R"({"event":"resource_snapshot")"
        << R"(,"node":")" << snap.node_id << "\""
        << R"(,"cpu_pct":)" << snap.cpu_usage_percent
        << R"(,"mem_avail_mb":)" << (snap.memory_available_bytes / (1024 * 1024))
        << R"(,"temp_c":)" << snap.cpu_temperature_celsius
        << R"(,"throttled":)" << (snap.is_throttled ? "true" : "false")
        << "}";
    emit(oss.str());
}

void MetricsCollector::record_scheduling_decision(const SchedulingDecision& decision) {
    std::ostringstream oss;
    oss << R"({"event":"scheduling_decision")"
        << R"(,"task":")" << decision.task_id << "\""
        << R"(,"assigned_to":")" << decision.assigned_node << "\""
        << R"(,"reason":)" << static_cast<int>(decision.reason)
        << "}";
    emit(oss.str());
}

void MetricsCollector::record_task_event(const TaskId& id, TaskState state, Duration duration) {
    std::ostringstream oss;
    oss << R"({"event":"task_state_change")"
        << R"(,"task":")" << id << "\""
        << R"(,"state":")" << to_string(state) << "\""
        << R"(,"duration_us":)" << duration.count()
        << "}";
    emit(oss.str());
}

void MetricsCollector::record_peer_event(const NodeId& peer, std::string_view event_type) {
    std::ostringstream oss;
    oss << R"({"event":"peer_)" << event_type << "\""
        << R"(,"peer":")" << peer << "\""
        << "}";
    emit(oss.str());
}

void MetricsCollector::record_custom(std::string_view event, std::string_view json_payload) {
    std::ostringstream oss;
    oss << R"({"event":")" << event << "\""
        << R"(,"data":)" << json_payload
        << "}";
    emit(oss.str());
}

/// Single writer lock: events from executor workers, discovery
/// callbacks, and the main loop interleave here, and NDJSON is only
/// parseable if lines never tear.
void MetricsCollector::emit(std::string_view json_line) {
    std::lock_guard lock(write_mutex_);
    sink_->write(json_line);
}

void MetricsCollector::flush() {
    std::lock_guard lock(write_mutex_);
    sink_->flush();
}

}  // namespace edge_orchestrator
