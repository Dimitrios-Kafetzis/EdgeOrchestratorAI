/**
 * @file result.hpp
 * @brief Monadic error handling type for EdgeOrchestrator.
 * @author Dimitris Kafetzis
 *
 * Provides Result<T, E> as the primary error-handling mechanism, avoiding
 * exceptions in performance-critical paths. Wraps std::expected (C++23)
 * with a fallback for C++20 compilers that lack it.
 */

#pragma once

#include <string>
#include <variant>
#include <optional>
#include <type_traits>
#include <utility>
#include <stdexcept>

namespace edge_orchestrator {

/**
 * @brief Error type carrying a descriptive message.
 */
struct Error {
    std::string message;

    explicit Error(std::string msg) : message(std::move(msg)) {}

    [[nodiscard]] const std::string& what() const noexcept { return message; }
};

/**
 * @brief Result<T, E> — a monadic error type.
 *
 * Holds either a success value of type T or an error of type E.
 * Designed as a lightweight, exception-free alternative for hot paths.
 *
 * @note When C++23 std::expected becomes widely available on target
 *       compilers, this can be replaced with a type alias.
 */
template <typename T, typename E = Error>
class Result {
public:
    // ── Constructors ──────────────────────────

    /// Construct a success result.
    Result(T value) : storage_(std::move(value)) {}  // NOLINT(implicit)

    /// Construct an error result.
    Result(E error) : storage_(std::move(error)) {}  // NOLINT(implicit)

    // ── Observers ─────────────────────────────

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] T& value() & {
        if (!has_value()) throw std::runtime_error("Result has no value");
        return std::get<T>(storage_);
    }

    [[nodiscard]] const T& value() const& {
        if (!has_value()) throw std::runtime_error("Result has no value");
        return std::get<T>(storage_);
    }

    [[nodiscard]] T&& value() && {
        if (!has_value()) throw std::runtime_error("Result has no value");
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] T& operator*() & { return value(); }
    [[nodiscard]] const T& operator*() const& { return value(); }
    [[nodiscard]] T* operator->() { return &value(); }
    [[nodiscard]] const T* operator->() const { return &value(); }

    [[nodiscard]] E& error() & {
        if (has_value()) throw std::runtime_error("Result has no error");
        return std::get<E>(storage_);
    }

    [[nodiscard]] const E& error() const& {
        if (has_value()) throw std::runtime_error("Result has no error");
        return std::get<E>(storage_);
    }

    // ── Monadic operations ────────────────────

    /// Transform the success value.
    template <typename F>
    auto map(F&& func) const -> Result<std::invoke_result_t<F, const T&>, E> {
        if (has_value()) {
            return func(value());
        }
        return error();
    }

    /// Chain with a function that returns a Result.
    template <typename F>
    auto and_then(F&& func) const -> std::invoke_result_t<F, const T&> {
        if (has_value()) {
            return func(value());
        }
        return error();
    }

    /// Provide a fallback value.
    [[nodiscard]] T value_or(T default_value) const& {
        if (has_value()) return value();
        return default_value;
    }

private:
    std::variant<T, E> storage_;
};

/**
 * @brief Specialization of Result for void success type.
 *
 * Used when an operation can fail but has no return value on success.
 */
template <typename E>
class Result<void, E> {
public:
    Result() : has_value_(true) {}
    Result(E error) : error_(std::move(error)), has_value_(false) {}  // NOLINT(implicit)

    [[nodiscard]] bool has_value() const noexcept { return has_value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value_; }

    [[nodiscard]] E& error() & {
        if (has_value_) throw std::runtime_error("Result has no error");
        return *error_;
    }

    [[nodiscard]] const E& error() const& {
        if (has_value_) throw std::runtime_error("Result has no error");
        return *error_;
    }

private:
    std::optional<E> error_;
    bool has_value_;
};

/// Convenience factory for error results.
template <typename T, typename E = Error>
Result<T, E> make_error(std::string message) {
    return Result<T, E>(E{std::move(message)});
}

}  // namespace edge_orchestrator
