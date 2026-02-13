/**
 * @file thread_pool.cpp
 * @brief ThreadPool implementation.
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
    // Request stop on all jthreads first
    for (auto& worker : workers_) {
        worker.request_stop();
    }
    // Wake all threads so they can observe the stop request
    queue_cv_.notify_all();
    // jthreads will auto-join in their destructors
}

void ThreadPool::worker_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::function<void(std::stop_token)> task;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, stop, [this] { return !task_queue_.empty(); });

            if (stop.stop_requested() && task_queue_.empty()) return;
            if (task_queue_.empty()) continue;

            task = std::move(task_queue_.front());
            task_queue_.pop();
        }

        ++active_tasks_;
        task(stop);
        --active_tasks_;
    }
}

size_t ThreadPool::active_count() const noexcept {
    return active_tasks_.load();
}

size_t ThreadPool::queued_count() const noexcept {
    std::lock_guard lock(queue_mutex_);
    return task_queue_.size();
}

size_t ThreadPool::thread_count() const noexcept {
    return workers_.size();
}

}  // namespace edge_orchestrator
