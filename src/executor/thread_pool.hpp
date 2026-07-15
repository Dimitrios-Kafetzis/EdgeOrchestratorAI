/**
 * @file thread_pool.hpp
 * @brief std::jthread-based thread pool with cooperative cancellation.
 * @author Dimitris Kafetzis
 *
 * Task hand-off is a lock-free bounded MPMC ring (mpmc_queue.hpp): the
 * submit path never takes a mutex, so producers (scheduler, network
 * server) cannot contend with each other or with workers. Idle workers
 * still sleep — a counting semaphore gates dequeues — because a spin
 * loop on an edge device is a battery and thermal bug, not a feature.
 */

#pragma once

#include "executor/mpmc_queue.hpp"

#include <concepts>
#include <functional>
#include <future>
#include <semaphore>
#include <thread>
#include <type_traits>
#include <vector>

namespace edge_orchestrator {

/**
 * @brief Thread pool using std::jthread for automatic join and stop_token support.
 */
class ThreadPool {
public:
    /// Bounded task backlog; try_push rejection is the backpressure signal.
    static constexpr size_t QUEUE_CAPACITY = 1024;

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
    using TaskFn = std::function<void(std::stop_token)>;

    void enqueue(TaskFn task);
    void worker_loop(std::stop_token stop);

    // Declaration order is load-bearing: workers_ must be declared LAST
    // so its destruction (jthread auto-join) runs FIRST — a worker must
    // never touch the queue or semaphore after they are destroyed. The
    // destructor also joins explicitly; this ordering is the backstop.
    MpmcQueue<TaskFn> task_queue_{QUEUE_CAPACITY};
    std::counting_semaphore<> tasks_available_{0};
    std::atomic<size_t> active_tasks_{0};
    std::vector<std::jthread> workers_;
};

// ── Template implementations ─────────────────

template <std::invocable F>
std::future<std::invoke_result_t<F>> ThreadPool::submit(F&& func) {
    using ReturnType = std::invoke_result_t<F>;
    auto promise = std::make_shared<std::promise<ReturnType>>();
    auto future = promise->get_future();

    enqueue([p = std::move(promise), f = std::forward<F>(func)](std::stop_token) mutable {
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
    return future;
}

template <std::invocable<std::stop_token> F>
std::future<std::invoke_result_t<F, std::stop_token>> ThreadPool::submit_cancellable(F&& func) {
    using ReturnType = std::invoke_result_t<F, std::stop_token>;
    auto promise = std::make_shared<std::promise<ReturnType>>();
    auto future = promise->get_future();

    enqueue([p = std::move(promise), f = std::forward<F>(func)](std::stop_token stop) mutable {
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
    return future;
}

}  // namespace edge_orchestrator
