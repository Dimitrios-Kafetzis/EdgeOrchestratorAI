/**
 * @file thread_pool.hpp
 * @brief std::jthread-based thread pool with cooperative cancellation.
 * @author Dimitris Kafetzis
 */

#pragma once

#include <concepts>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

namespace edge_orchestrator {

/**
 * @brief Thread pool using std::jthread for automatic join and stop_token support.
 */
class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads = 0);
    ~ThreadPool();

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /// Submit a callable for execution.
    template <std::invocable F>
    std::future<std::invoke_result_t<F>> submit(F&& func);

    /// Submit a callable that accepts a stop_token.
    template <std::invocable<std::stop_token> F>
    std::future<std::invoke_result_t<F, std::stop_token>> submit_cancellable(F&& func);

    [[nodiscard]] size_t active_count() const noexcept;
    [[nodiscard]] size_t queued_count() const noexcept;
    [[nodiscard]] size_t thread_count() const noexcept;

private:
    void worker_loop(std::stop_token stop);

    std::vector<std::jthread> workers_;
    std::queue<std::function<void(std::stop_token)>> task_queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable_any queue_cv_;
    std::atomic<size_t> active_tasks_{0};
};

// ── Template implementations ─────────────────

template <std::invocable F>
std::future<std::invoke_result_t<F>> ThreadPool::submit(F&& func) {
    using ReturnType = std::invoke_result_t<F>;
    auto promise = std::make_shared<std::promise<ReturnType>>();
    auto future = promise->get_future();

    {
        std::lock_guard lock(queue_mutex_);
        task_queue_.push([p = std::move(promise), f = std::forward<F>(func)](std::stop_token) mutable {
            try {
                if constexpr (std::is_void_v<ReturnType>) {
                    f();
                    p->set_value();
                } else {
                    p->set_value(f());
                }
            } catch (...) {
                p->set_exception(std::current_exception());
            }
        });
    }
    queue_cv_.notify_one();
    return future;
}

template <std::invocable<std::stop_token> F>
std::future<std::invoke_result_t<F, std::stop_token>> ThreadPool::submit_cancellable(F&& func) {
    using ReturnType = std::invoke_result_t<F, std::stop_token>;
    auto promise = std::make_shared<std::promise<ReturnType>>();
    auto future = promise->get_future();

    {
        std::lock_guard lock(queue_mutex_);
        task_queue_.push([p = std::move(promise), f = std::forward<F>(func)](std::stop_token stop) mutable {
            try {
                if constexpr (std::is_void_v<ReturnType>) {
                    f(stop);
                    p->set_value();
                } else {
                    p->set_value(f(stop));
                }
            } catch (...) {
                p->set_exception(std::current_exception());
            }
        });
    }
    queue_cv_.notify_one();
    return future;
}

}  // namespace edge_orchestrator
