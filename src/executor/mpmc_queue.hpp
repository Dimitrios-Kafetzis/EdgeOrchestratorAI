/**
 * @file mpmc_queue.hpp
 * @brief Bounded lock-free multi-producer multi-consumer queue.
 * @author Dimitris Kafetzis
 *
 * Dmitry Vyukov's bounded MPMC algorithm: a power-of-two ring of cells,
 * each carrying an atomic sequence number that encodes whose turn the
 * cell is on. Producers and consumers claim positions with a single CAS
 * and never block each other; there are no locks and no unbounded loops
 * while holding shared state.
 *
 * Bounded on purpose: on an edge node an unbounded task queue is a slow
 * OOM. Rejection (try_push returning false) is the backpressure signal.
 */

#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <new>

namespace edge_orchestrator {

template <typename T>
class MpmcQueue {
public:
    /// Capacity is rounded up to the next power of two (minimum 2).
    explicit MpmcQueue(size_t capacity)
        : capacity_(std::bit_ceil(capacity < 2 ? size_t{2} : capacity)),
          mask_(capacity_ - 1),
          cells_(std::make_unique<Cell[]>(capacity_)) {
        for (size_t i = 0; i < capacity_; ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    MpmcQueue(const MpmcQueue&) = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    /// Non-blocking. Returns false when the queue is full.
    bool try_push(T&& value) {
        Cell* cell;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            auto diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                // The cell is free on this lap; claim it.
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1,
                                                       std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // full: the cell still holds last lap's item
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        cell->value = std::move(value);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    /// Non-blocking. Returns false when the queue is empty.
    bool try_pop(T& out) {
        Cell* cell;
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            auto diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1,
                                                       std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // empty: no producer has filled this cell yet
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        out = std::move(cell->value);
        // Mark the cell free for this position's next lap.
        cell->sequence.store(pos + capacity_, std::memory_order_release);
        return true;
    }

    /// Approximate: racy by nature, useful for telemetry only.
    [[nodiscard]] size_t size_approx() const noexcept {
        auto head = enqueue_pos_.load(std::memory_order_relaxed);
        auto tail = dequeue_pos_.load(std::memory_order_relaxed);
        return head >= tail ? head - tail : 0;
    }

    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

private:
    struct Cell {
        std::atomic<size_t> sequence;
        T value;
    };

    static constexpr size_t CACHELINE = 64;

    const size_t capacity_;
    const size_t mask_;
    std::unique_ptr<Cell[]> cells_;

    // Producers and consumers hammer different counters; keep them on
    // separate cache lines so they don't false-share.
    alignas(CACHELINE) std::atomic<size_t> enqueue_pos_{0};
    alignas(CACHELINE) std::atomic<size_t> dequeue_pos_{0};
};

}  // namespace edge_orchestrator
