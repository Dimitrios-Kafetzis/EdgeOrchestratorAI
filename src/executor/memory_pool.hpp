/**
 * @file memory_pool.hpp
 * @brief Arena-style memory pool for zero-allocation execution hot paths.
 * @author Dimitris Kafetzis
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace edge_orchestrator {

/**
 * @brief Fast arena allocator for task execution.
 *
 * Allocates linearly from a pre-allocated buffer. All allocations are
 * freed at once via reset(). Thread-safe via atomic offset.
 */
class MemoryPool {
public:
    explicit MemoryPool(size_t capacity_bytes);

    [[nodiscard]] void* allocate(size_t size, size_t alignment = alignof(std::max_align_t));
    void reset() noexcept;

    [[nodiscard]] size_t used() const noexcept;
    [[nodiscard]] size_t capacity() const noexcept;
    [[nodiscard]] bool can_allocate(size_t size) const noexcept;

private:
    std::unique_ptr<std::byte[]> buffer_;
    size_t capacity_;
    std::atomic<size_t> offset_{0};
};

}  // namespace edge_orchestrator
