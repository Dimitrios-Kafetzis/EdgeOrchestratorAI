/**
 * @file metrics_collector.hpp
 * @brief Structured event collection for telemetry.
 * @author Dimitris Kafetzis
 */

#pragma once

#include "core/logger.hpp"
#include "core/types.hpp"
#include "scheduler/scheduler.hpp"

#include <memory>
#include <mutex>

namespace edge_orchestrator {

/**
 * @brief Collects and logs structured telemetry events as NDJSON.
 */
class MetricsCollector {
public:
    explicit MetricsCollector(std::unique_ptr<ILogSink> sink);

    void record_resource_snapshot(const ResourceSnapshot& snap);
    void record_scheduling_decision(const SchedulingDecision& decision);
    void record_task_event(const TaskId& id, TaskState state, Duration duration);
    void record_peer_event(const NodeId& peer, std::string_view event_type);
    void record_custom(std::string_view event, std::string_view json_payload);

    void flush();

private:
    std::unique_ptr<ILogSink> sink_;
    std::mutex write_mutex_;

    void emit(std::string_view json_line);
};

}  // namespace edge_orchestrator
