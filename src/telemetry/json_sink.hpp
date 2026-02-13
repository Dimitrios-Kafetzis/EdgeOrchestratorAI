/**
 * @file json_sink.hpp
 * @brief NDJSON file log sink with rotation support.
 * @author Dimitris Kafetzis
 */

#pragma once

#include "core/logger.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace edge_orchestrator {

/**
 * @brief Writes NDJSON to rotating log files.
 */
class JsonFileSink : public ILogSink {
public:
    JsonFileSink(const std::filesystem::path& log_dir,
                 const std::string& prefix,
                 uint32_t max_file_size_mb = 50,
                 uint32_t max_files = 5);
    ~JsonFileSink() override;

    void write(std::string_view json_line) override;
    void flush() override;

private:
    void rotate_if_needed();

    std::filesystem::path log_dir_;
    std::string prefix_;
    uint32_t max_file_size_bytes_;
    uint32_t max_files_;
    std::ofstream current_file_;
    size_t current_size_{0};
};

/**
 * @brief Writes to stdout — useful for development/debugging.
 */
class StdoutSink : public ILogSink {
public:
    void write(std::string_view json_line) override;
    void flush() override;
};

/**
 * @brief Discards all output — useful for benchmarking.
 */
class NullSink : public ILogSink {
public:
    void write(std::string_view /*json_line*/) override {}
    void flush() override {}
};

}  // namespace edge_orchestrator
