/**
 * @file mock_monitor.cpp
 * @brief MockMonitor implementation — configurable resource snapshots for testing.
 * @author Dimitris Kafetzis
 */

#include "resource_monitor/monitor.hpp"

namespace edge_orchestrator {

MockMonitor::MockMonitor(NodeId node_id, uint32_t /*sampling_interval_ms*/)
    : node_id_(std::move(node_id)) {
    // Sensible defaults resembling a Raspberry Pi 4
    static_snapshot_.node_id = node_id_;
    static_snapshot_.memory_total_bytes = 4ULL * 1024 * 1024 * 1024;  // 4 GB
    static_snapshot_.memory_available_bytes = 2ULL * 1024 * 1024 * 1024;  // 2 GB
    static_snapshot_.cpu_usage_percent = 25.0f;
    static_snapshot_.per_core_cpu_percent = {25.0f, 25.0f, 25.0f, 25.0f};
    static_snapshot_.cpu_temperature_celsius = 45.0f;
    static_snapshot_.is_throttled = false;
}

Result<ResourceSnapshot> MockMonitor::read() {
    if (use_static_) {
        static_snapshot_.timestamp = std::chrono::system_clock::now();
        return static_snapshot_;
    }
    if (index_ >= sequence_.size()) {
        return Error{"Mock sequence exhausted"};
    }
    auto snap = sequence_[index_++];
    snap.timestamp = std::chrono::system_clock::now();
    return snap;
}

float MockMonitor::cpu_usage() {
    return use_static_ ? static_snapshot_.cpu_usage_percent
                       : (index_ > 0 && index_ <= sequence_.size()
                              ? sequence_[index_ - 1].cpu_usage_percent
                              : 0.0f);
}

uint64_t MockMonitor::memory_available() {
    return use_static_ ? static_snapshot_.memory_available_bytes
                       : (index_ > 0 && index_ <= sequence_.size()
                              ? sequence_[index_ - 1].memory_available_bytes
                              : 0);
}

bool MockMonitor::is_throttled() {
    return use_static_ ? static_snapshot_.is_throttled
                       : (index_ > 0 && index_ <= sequence_.size()
                              ? sequence_[index_ - 1].is_throttled
                              : false);
}

void MockMonitor::start() { /* no-op for mock */ }
void MockMonitor::stop()  { /* no-op for mock */ }

void MockMonitor::push_snapshot(ResourceSnapshot snapshot) {
    use_static_ = false;
    snapshot.node_id = node_id_;
    sequence_.push_back(std::move(snapshot));
}

void MockMonitor::set_static_snapshot(ResourceSnapshot snapshot) {
    use_static_ = true;
    snapshot.node_id = node_id_;
    static_snapshot_ = std::move(snapshot);
}

void MockMonitor::set_cpu(float percent) {
    static_snapshot_.cpu_usage_percent = percent;
    static_snapshot_.per_core_cpu_percent.fill(percent);
}

void MockMonitor::set_memory(uint64_t available, uint64_t total) {
    static_snapshot_.memory_available_bytes = available;
    static_snapshot_.memory_total_bytes = total;
}

void MockMonitor::set_throttled(bool throttled) {
    static_snapshot_.is_throttled = throttled;
}

}  // namespace edge_orchestrator
