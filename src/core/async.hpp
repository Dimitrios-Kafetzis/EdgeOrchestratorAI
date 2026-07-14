/**
 * @file async.hpp
 * @brief Minimal C++20 coroutine task types for async I/O.
 * @author Dimitris Kafetzis
 *
 * Async<T> is a lazy coroutine: it starts suspended and runs when awaited,
 * resuming its awaiter on completion (symmetric transfer, no recursion).
 * DetachedTask is fire-and-forget for top-level coroutines owned by the
 * reactor (accept loops, per-connection handlers): it starts immediately
 * and destroys its own frame at final suspend.
 */

#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

namespace edge_orchestrator {

template <typename T>
class Async {
public:
    struct promise_type {
        std::optional<T> value;
        std::exception_ptr exception;
        std::coroutine_handle<> continuation;

        Async get_return_object() {
            return Async{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) noexcept {
                // Resume whoever awaited us; end the chain otherwise.
                auto cont = h.promise().continuation;
                return cont ? cont : std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_value(T v) { value = std::move(v); }
        void unhandled_exception() { exception = std::current_exception(); }
    };

    Async() = default;
    explicit Async(std::coroutine_handle<promise_type> h) : handle_(h) {}
    Async(Async&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Async& operator=(Async&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }
    Async(const Async&) = delete;
    Async& operator=(const Async&) = delete;
    ~Async() {
        if (handle_) handle_.destroy();
    }

    bool await_ready() const noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiter) noexcept {
        handle_.promise().continuation = awaiter;
        return handle_;  // start the child coroutine
    }
    T await_resume() {
        if (handle_.promise().exception) {
            std::rethrow_exception(handle_.promise().exception);
        }
        return std::move(*handle_.promise().value);
    }

private:
    std::coroutine_handle<promise_type> handle_;
};

/**
 * @brief Fire-and-forget coroutine. Starts eagerly; frame self-destroys.
 *
 * Used for reactor-owned coroutines whose lifetime nothing awaits. The
 * coroutine must not outlive the objects it captures — the transport
 * guarantees this by draining the reactor before teardown.
 */
struct DetachedTask {
    struct promise_type {
        DetachedTask get_return_object() { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

}  // namespace edge_orchestrator
