/**
 * @file logger.hpp
 * @brief Logging infrastructure with pluggable sinks.
 * @author Dimitris Kafetzis
 *
 * Provides ILogSink (virtual interface for runtime-configurable log destinations)
 * and a thread-safe Logger front-end. Log sinks use virtual dispatch because
 * they are configured once at startup and not on the hot path.
 */

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace edge_orchestrator {

// ─────────────────────────────────────────────
// Log Levels
// ─────────────────────────────────────────────

enum class LogLevel : uint8_t {
    Debug,
    Info,
    Warn,
    Error
};

[[nodiscard]] constexpr std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
    }
    return "unknown";
}

// ─────────────────────────────────────────────
// ILogSink (Virtual — runtime-configurable)
// ─────────────────────────────────────────────

/**
 * @brief Abstract interface for log output destinations.
 *
 * Virtual dispatch is acceptable here because logging is I/O-bound,
 * not on the scheduling/execution hot path.
 */
class ILogSink {
public:
    virtual ~ILogSink() = default;

    virtual void write(std::string_view json_line) = 0;
    virtual void flush() = 0;
};

// ─────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────

/**
 * @brief Thread-safe logger front-end.
 */
class Logger {
public:
    explicit Logger(std::unique_ptr<ILogSink> sink, LogLevel min_level = LogLevel::Info);

    void debug(std::string_view message);
    void info(std::string_view message);
    void warn(std::string_view message);
    void error(std::string_view message);

    void log(LogLevel level, std::string_view message);
    void flush();

    void set_level(LogLevel level) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;

private:
    std::unique_ptr<ILogSink> sink_;
    LogLevel min_level_;
    mutable std::mutex mutex_;
};

}  // namespace edge_orchestrator
