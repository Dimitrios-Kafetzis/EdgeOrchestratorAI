/**
 * @file memory_pool.cpp
 * @brief MemoryPool implementation — a lock-free bump arena.
 * @author Dimitris Kafetzis
 *
 * allocate() is a CAS loop over a single offset: align, bounds-check,
 * claim. No per-allocation headers, no free lists, no fragmentation —
 * and no free() either. Memory is reclaimed wholesale by reset() at
 * workload boundaries (Orchestrator::try_reset_arena holds the
 * exclusive arena lock for that, so a reset can never yank memory from
 * under a running task). The trade-off is deliberate for this workload
 * shape: tasks allocate scratch buffers, run, and die together, so
 * arena semantics buy O(1) allocation with zero contention on the
 * happy path and make the executor's memory ceiling a hard number
 * (capacity_) instead of a hope.
 */

#include "executor/memory_pool.hpp"

#include <cassert>
#include <cstring>

namespace edge_orchestrator {

MemoryPool::MemoryPool(size_t capacity_bytes)
    : buffer_(std::make_unique<std::byte[]>(capacity_bytes))
    , capacity_(capacity_bytes) {
    std::memset(buffer_.get(), 0, capacity_bytes);
}

/// Returns nullptr when the arena can't fit the request — the caller
/// (TaskRunner) turns that into a failed task, not an exception: on an
/// 8 GB edge node, over-budget must be a scheduling signal, not a crash.
void* MemoryPool::allocate(size_t size, size_t alignment) {
    size_t current = offset_.load(std::memory_order_relaxed);
    size_t aligned;

    do {
        aligned = (current + alignment - 1) & ~(alignment - 1);
        if (aligned + size > capacity_) return nullptr;
    } while (!offset_.compare_exchange_weak(current, aligned + size,
                                             std::memory_order_release,
                                             std::memory_order_relaxed));

    return buffer_.get() + aligned;
}

void MemoryPool::reset() noexcept {
    offset_.store(0, std::memory_order_release);
}

size_t MemoryPool::used() const noexcept {
    return offset_.load(std::memory_order_acquire);
}

size_t MemoryPool::capacity() const noexcept {
    return capacity_;
}

bool MemoryPool::can_allocate(size_t size) const noexcept {
    return (used() + size) <= capacity_;
}

}  // namespace edge_orchestrator
