/**
 * @file thread_pool.cpp
 * @brief ThreadPool implementation over the lock-free MPMC queue.
 * @author Dimitris Kafetzis
 */

#include "executor/thread_pool.hpp"

namespace edge_orchestrator {

ThreadPool::ThreadPool(size_t num_threads) {
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 4;  // fallback
    }

    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this](std::stop_token stop) {
            worker_loop(stop);
        });
    }
}

ThreadPool::~ThreadPool() {
    for (auto& worker : workers_) {
        worker.request_stop();
    }
    // One permit per worker so every sleeper wakes and observes the stop.
    tasks_available_.release(static_cast<std::ptrdiff_t>(workers_.size()));
    // Join HERE, in the destructor body: waiting for jthread auto-join
    // would destroy the semaphore and queue first (reverse member
    // order), leaving workers racing against dead objects.
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

void ThreadPool::enqueue(TaskFn task) {
    // The ring rejects when full; yielding here turns rejection into
    // backpressure on the producer instead of unbounded memory growth.
    while (!task_queue_.try_push(std::move(task))) {
        std::this_thread::yield();
    }
    tasks_available_.release();
}

void ThreadPool::worker_loop(std::stop_token stop) {
    while (true) {
        tasks_available_.acquire();

        TaskFn task;
        if (task_queue_.try_pop(task)) {
            ++active_tasks_;
            task(stop);
            --active_tasks_;
            continue;
        }

        // No task behind the permit: it was a shutdown wake-up.
        if (stop.stop_requested()) return;
    }
}

size_t ThreadPool::active_count() const noexcept {
    return active_tasks_.load();
}

size_t ThreadPool::queued_count() const noexcept {
    return task_queue_.size_approx();
}

size_t ThreadPool::thread_count() const noexcept {
    return workers_.size();
}

}  // namespace edge_orchestrator
