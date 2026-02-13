/**
 * @file linux_monitor.cpp
 * @brief LinuxMonitor — reads CPU, memory, thermal, and network metrics
 *        from Linux pseudo-filesystems (/proc, /sys).
 * @author Dimitris Kafetzis
 *
 * Sampling is performed by a dedicated std::jthread at a configurable
 * interval. The latest snapshot is published atomically for lock-free
 * reads by the scheduler.
 */

#include "resource_monitor/monitor.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace edge_orchestrator {

// Use the header-defined internal types
using CpuTimes = LinuxMonitor::CpuTimesInternal;
using NetCounters = LinuxMonitor::NetCountersInternal;

// ─────────────────────────────────────────────
// Internal helpers for /proc and /sys parsing
// ─────────────────────────────────────────────
namespace {

std::string read_file_line(const std::string& path) {
    std::ifstream ifs(path);
    std::string line;
    if (ifs.is_open()) {
        std::getline(ifs, line);
    }
    return line;
}

std::vector<std::string> read_file_lines(const std::string& path) {
    std::ifstream ifs(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ifs, line)) {
        lines.push_back(std::move(line));
    }
    return lines;
}

/**
 * @brief Parse a CPU line from /proc/stat.
 * Format: "cpu[N] user nice system idle iowait irq softirq steal ..."
 */
CpuTimes parse_cpu_line(const std::string& line) {
    CpuTimes times;
    std::istringstream iss(line);
    std::string label;
    iss >> label >> times.user >> times.nice >> times.system >> times.idle
        >> times.iowait >> times.irq >> times.softirq >> times.steal;
    return times;
}

float compute_cpu_percent(const CpuTimes& prev, const CpuTimes& curr) {
    auto prev_total = prev.user + prev.nice + prev.system + prev.idle
                    + prev.iowait + prev.irq + prev.softirq + prev.steal;
    auto curr_total = curr.user + curr.nice + curr.system + curr.idle
                    + curr.iowait + curr.irq + curr.softirq + curr.steal;
    auto prev_active = prev.user + prev.nice + prev.system
                     + prev.irq + prev.softirq + prev.steal;
    auto curr_active = curr.user + curr.nice + curr.system
                     + curr.irq + curr.softirq + curr.steal;

    uint64_t total_delta = curr_total - prev_total;
    if (total_delta == 0) return 0.0f;
    uint64_t active_delta = curr_active - prev_active;
    return 100.0f * static_cast<float>(active_delta) / static_cast<float>(total_delta);
}

struct MemInfo {
    uint64_t total_kb{0};
    uint64_t available_kb{0};
};

MemInfo parse_meminfo() {
    MemInfo info;
    auto lines = read_file_lines("/proc/meminfo");
    for (const auto& line : lines) {
        if (line.starts_with("MemTotal:")) {
            std::istringstream iss(line.substr(9));
            iss >> info.total_kb;
        } else if (line.starts_with("MemAvailable:")) {
            std::istringstream iss(line.substr(13));
            iss >> info.available_kb;
        }
    }
    return info;
}

float read_temperature(const std::string& zone_path) {
    auto line = read_file_line(zone_path);
    if (line.empty()) return 0.0f;
    try {
        return static_cast<float>(std::stol(line)) / 1000.0f;
    } catch (...) {
        return 0.0f;
    }
}

NetCounters parse_net_dev() {
    NetCounters total;
    auto lines = read_file_lines("/proc/net/dev");
    for (size_t i = 2; i < lines.size(); ++i) {
        const auto& line = lines[i];
        auto colon_pos = line.find(':');
        if (colon_pos == std::string::npos) continue;

        std::string iface = line.substr(0, colon_pos);
        auto start = iface.find_first_not_of(' ');
        if (start != std::string::npos) iface = iface.substr(start);
        if (iface == "lo") continue;

        std::istringstream iss(line.substr(colon_pos + 1));
        uint64_t rx_bytes, rx_p, rx_e, rx_d, rx_fi, rx_fr, rx_c, rx_m, tx_bytes;
        iss >> rx_bytes >> rx_p >> rx_e >> rx_d >> rx_fi >> rx_fr >> rx_c >> rx_m >> tx_bytes;

        total.rx_bytes += rx_bytes;
        total.tx_bytes += tx_bytes;
    }
    return total;
}

bool read_rpi_throttled() {
    auto line = read_file_line("/sys/devices/platform/soc/soc:firmware/get_throttled");
    if (line.empty()) return false;
    try {
        uint64_t val = std::stoull(line, nullptr, 0);
        return (val & 0x4) != 0;
    } catch (...) {
        return false;
    }
}

}  // anonymous namespace

// ─────────────────────────────────────────────
// LinuxMonitor implementation
// ─────────────────────────────────────────────

LinuxMonitor::LinuxMonitor(NodeId node_id, uint32_t sampling_interval_ms)
    : node_id_(std::move(node_id)), interval_ms_(sampling_interval_ms) {}

LinuxMonitor::~LinuxMonitor() {
    stop();
}

void LinuxMonitor::start() {
    prev_cpu_times_ = {};
    prev_per_core_times_.fill({});
    prev_net_ = parse_net_dev();

    auto lines = read_file_lines("/proc/stat");
    if (!lines.empty()) {
        prev_cpu_times_ = parse_cpu_line(lines[0]);
        for (size_t i = 0; i < 4 && (i + 1) < lines.size(); ++i) {
            if (lines[i + 1].starts_with("cpu")) {
                prev_per_core_times_[i] = parse_cpu_line(lines[i + 1]);
            }
        }
    }

    sampling_thread_ = std::jthread([this](std::stop_token stop) {
        sampling_loop(stop);
    });
}

void LinuxMonitor::stop() {
    if (sampling_thread_.joinable()) {
        sampling_thread_.request_stop();
    }
}

Result<ResourceSnapshot> LinuxMonitor::read() {
    auto snapshot = latest_.load();
    if (!snapshot) {
        return Error{"No snapshot available yet"};
    }
    return *snapshot;
}

float LinuxMonitor::cpu_usage() {
    auto snapshot = latest_.load();
    return snapshot ? snapshot->cpu_usage_percent : 0.0f;
}

uint64_t LinuxMonitor::memory_available() {
    auto snapshot = latest_.load();
    return snapshot ? snapshot->memory_available_bytes : 0;
}

bool LinuxMonitor::is_throttled() {
    auto snapshot = latest_.load();
    return snapshot ? snapshot->is_throttled : false;
}

void LinuxMonitor::on_cpu_threshold(float percent, ThresholdCallback cb) {
    std::lock_guard lock(callbacks_mutex_);
    cpu_callbacks_.push_back({percent, std::move(cb)});
}

void LinuxMonitor::on_memory_threshold(float percent, ThresholdCallback cb) {
    std::lock_guard lock(callbacks_mutex_);
    memory_callbacks_.push_back({percent, std::move(cb)});
}

void LinuxMonitor::sampling_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        auto snapshot = sample_once();
        auto shared = std::make_shared<ResourceSnapshot>(snapshot);
        latest_.store(shared);
        check_thresholds(snapshot);
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
    }
}

ResourceSnapshot LinuxMonitor::sample_once() {
    ResourceSnapshot snap;
    snap.node_id = node_id_;
    snap.timestamp = std::chrono::system_clock::now();

    // CPU Usage
    auto stat_lines = read_file_lines("/proc/stat");
    if (!stat_lines.empty()) {
        auto curr = parse_cpu_line(stat_lines[0]);
        snap.cpu_usage_percent = compute_cpu_percent(prev_cpu_times_, curr);
        prev_cpu_times_ = curr;

        for (size_t i = 0; i < 4 && (i + 1) < stat_lines.size(); ++i) {
            if (stat_lines[i + 1].starts_with("cpu")) {
                auto core_curr = parse_cpu_line(stat_lines[i + 1]);
                snap.per_core_cpu_percent[i] =
                    compute_cpu_percent(prev_per_core_times_[i], core_curr);
                prev_per_core_times_[i] = core_curr;
            }
        }
    }

    // Memory
    auto mem = parse_meminfo();
    snap.memory_total_bytes = mem.total_kb * 1024;
    snap.memory_available_bytes = mem.available_kb * 1024;

    // Temperature
    snap.cpu_temperature_celsius =
        read_temperature("/sys/class/thermal/thermal_zone0/temp");
    snap.gpu_temperature_celsius =
        read_temperature("/sys/class/thermal/thermal_zone1/temp");

    // Network I/O
    auto curr_net = parse_net_dev();
    float interval_sec = static_cast<float>(interval_ms_) / 1000.0f;
    if (interval_sec > 0.0f) {
        snap.network_rx_bytes_sec = static_cast<uint64_t>(
            static_cast<float>(curr_net.rx_bytes - prev_net_.rx_bytes) / interval_sec);
        snap.network_tx_bytes_sec = static_cast<uint64_t>(
            static_cast<float>(curr_net.tx_bytes - prev_net_.tx_bytes) / interval_sec);
    }
    prev_net_ = curr_net;

    // Thermal throttling
    snap.is_throttled = read_rpi_throttled();

    return snap;
}

void LinuxMonitor::check_thresholds(const ResourceSnapshot& snap) {
    std::lock_guard lock(callbacks_mutex_);
    for (const auto& [threshold, cb] : cpu_callbacks_) {
        if (snap.cpu_usage_percent >= threshold) {
            cb(snap);
        }
    }
    for (const auto& [threshold, cb] : memory_callbacks_) {
        if (snap.memory_usage_percent() >= threshold) {
            cb(snap);
        }
    }
}

}  // namespace edge_orchestrator
