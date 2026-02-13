/**
 * @file logger.cpp
 * @brief Logger implementation with ISO 8601 timestamps.
 * @author Dimitris Kafetzis
 */

#include "core/logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace edge_orchestrator {

Logger::Logger(std::unique_ptr<ILogSink> sink, LogLevel min_level)
    : sink_(std::move(sink)), min_level_(min_level) {}

void Logger::debug(std::string_view message) { log(LogLevel::Debug, message); }
void Logger::info(std::string_view message)  { log(LogLevel::Info, message); }
void Logger::warn(std::string_view message)  { log(LogLevel::Warn, message); }
void Logger::error(std::string_view message) { log(LogLevel::Error, message); }

void Logger::log(LogLevel level, std::string_view message) {
    if (level < min_level_) return;

    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << R"({"level":")" << to_string(level) << R"(",)"
        << R"("ts":")"
        << std::put_time(std::gmtime(&time_t_now), "%FT%T")
        << '.' << std::setfill('0') << std::setw(3) << ms.count()
        << R"(Z",)"
        << R"("msg":")" << message << R"("})";

    std::lock_guard lock(mutex_);
    sink_->write(oss.str());
}

void Logger::flush() {
    std::lock_guard lock(mutex_);
    sink_->flush();
}

void Logger::set_level(LogLevel level) noexcept { min_level_ = level; }
LogLevel Logger::level() const noexcept { return min_level_; }

}  // namespace edge_orchestrator
