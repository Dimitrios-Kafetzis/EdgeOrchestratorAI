/**
 * @file memory_pool.cpp
 * @brief MemoryPool implementation.
 * @author Dimitris Kafetzis
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
