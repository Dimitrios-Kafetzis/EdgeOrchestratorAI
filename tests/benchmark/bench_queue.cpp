/**
 * @file bench_queue.cpp
 * @brief Lock-free MPMC queue vs mutex-guarded std::queue.
 * @author Dimitris Kafetzis
 *
 * Measures throughput of the task hand-off path under contention. The
 * mutex baseline replicates the ThreadPool's previous implementation so
 * the comparison is honest: same payload type, same producers/consumers.
 *
 * Usage: ./bench_queue [ops_per_producer]
 */

#include "executor/mpmc_queue.hpp"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

using namespace edge_orchestrator;
using Clock = std::chrono::steady_clock;

namespace {

/// The ThreadPool's previous hand-off: std::queue + mutex.
class MutexQueue {
public:
    bool try_push(int&& v) {
        std::lock_guard lock(mutex_);
        queue_.push(v);
        return true;
    }
    bool try_pop(int& out) {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) return false;
        out = queue_.front();
        queue_.pop();
        return true;
    }

private:
    std::queue<int> queue_;
    std::mutex mutex_;
};

template <typename Queue>
double run_benchmark(Queue& q, int producers, int consumers, int per_producer) {
    std::atomic<int> consumed{0};
    const int total = producers * per_producer;

    auto start = Clock::now();
    {
        std::vector<std::jthread> threads;
        for (int c = 0; c < consumers; ++c) {
            threads.emplace_back([&]() {
                int out;
                while (consumed.load(std::memory_order_relaxed) < total) {
                    if (q.try_pop(out)) {
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (int p = 0; p < producers; ++p) {
            threads.emplace_back([&]() {
                for (int i = 0; i < per_producer; ++i) {
                    int v = i;
                    while (!q.try_push(std::move(v))) {
                        v = i;
                    }
                }
            });
        }
    }  // join
    auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    return static_cast<double>(total) / elapsed / 1e6;  // Mops/s
}

}  // namespace

int main(int argc, char* argv[]) {
    const int per_producer = argc > 1 ? std::stoi(argv[1]) : 250000;

    std::cout << "Queue hand-off throughput (Mops/s, higher is better)\n";
    std::cout << "ops/producer: " << per_producer << "\n\n";
    std::cout << std::left << std::setw(12) << "P x C"
              << std::setw(14) << "mutex+queue"
              << std::setw(14) << "lock-free"
              << "speedup\n";

    for (auto [p, c] : {std::pair{1, 1}, {2, 2}, {4, 4}, {8, 8}}) {
        MutexQueue mq;
        double mutex_mops = run_benchmark(mq, p, c, per_producer);

        MpmcQueue<int> lfq(1024);
        double lf_mops = run_benchmark(lfq, p, c, per_producer);

        std::cout << std::left << std::setw(12)
                  << (std::to_string(p) + " x " + std::to_string(c))
                  << std::setw(14) << std::fixed << std::setprecision(2) << mutex_mops
                  << std::setw(14) << lf_mops
                  << std::setprecision(1) << (lf_mops / mutex_mops) << "x\n";
    }
    return 0;
}
