/**
 * @file json_sink.cpp
 * @brief Log sink implementations.
 * @author Dimitris Kafetzis
 */

#include "telemetry/json_sink.hpp"

#include <iostream>

namespace edge_orchestrator {

// ── JsonFileSink ─────────────────────────────

JsonFileSink::JsonFileSink(const std::filesystem::path& log_dir,
                            const std::string& prefix,
                            uint32_t max_file_size_mb,
                            uint32_t max_files)
    : log_dir_(log_dir)
    , prefix_(prefix)
    , max_file_size_bytes_(max_file_size_mb * 1024 * 1024)
    , max_files_(max_files) {
    std::filesystem::create_directories(log_dir_);
    auto path = log_dir_ / (prefix_ + ".ndjson");
    current_file_.open(path, std::ios::app);
}

JsonFileSink::~JsonFileSink() {
    if (current_file_.is_open()) {
        current_file_.flush();
        current_file_.close();
    }
}

void JsonFileSink::write(std::string_view json_line) {
    rotate_if_needed();
    if (current_file_.is_open()) {
        current_file_ << json_line << '\n';
        current_size_ += json_line.size() + 1;
    }
}

void JsonFileSink::flush() {
    if (current_file_.is_open()) {
        current_file_.flush();
    }
}

void JsonFileSink::rotate_if_needed() {
    if (current_size_ < max_file_size_bytes_) return;
    // TODO: Implement file rotation
}

// ── StdoutSink ───────────────────────────────

void StdoutSink::write(std::string_view json_line) {
    std::cout << json_line << '\n';
}

void StdoutSink::flush() {
    std::cout.flush();
}

}  // namespace edge_orchestrator
