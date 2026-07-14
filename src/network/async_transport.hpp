/**
 * @file async_transport.hpp
 * @brief Coroutine-based async TCP server over an epoll reactor.
 * @author Dimitris Kafetzis
 *
 * Serves the same wire contract as TcpTransport — [4B BE length][payload],
 * 16 MB cap, one request/response per connection — but handles many
 * connections concurrently on one reactor thread. A slow request (e.g. a
 * whole workload submission) no longer blocks the next peer's offload.
 *
 * Handlers do not run on the reactor thread: each connection coroutine
 * hops to a small worker pool for the handler call, then back to the
 * reactor for socket I/O. The client side of offloading deliberately
 * stays synchronous (TcpTransport) — it already runs on an executor
 * worker that owns the operation, so async buys nothing there.
 */

#pragma once

#include "core/result.hpp"
#include "core/async.hpp"
#include "executor/thread_pool.hpp"
#include "network/reactor.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace edge_orchestrator {

class AsyncTransport {
public:
    static constexpr uint32_t MAX_MESSAGE_SIZE = 16 * 1024 * 1024;  // 16 MB
    static constexpr int DEFAULT_BACKLOG = 16;

    using MessageHandler = std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)>;

    explicit AsyncTransport(size_t handler_threads = 2,
                            uint32_t io_timeout_ms = 10000);
    ~AsyncTransport();

    AsyncTransport(const AsyncTransport&) = delete;
    AsyncTransport& operator=(const AsyncTransport&) = delete;

    Result<void> listen(uint16_t port, int backlog = DEFAULT_BACKLOG);
    void serve(MessageHandler handler);
    void stop_serving();

    [[nodiscard]] bool is_listening() const noexcept { return server_fd_ >= 0; }
    [[nodiscard]] size_t active_connections() const noexcept {
        return active_connections_.load();
    }

private:
    /// Awaitable that moves the coroutine onto the handler pool.
    struct PoolHop {
        ThreadPool& pool;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle) {
            pool.submit([handle]() mutable { handle.resume(); });
        }
        void await_resume() const noexcept {}
    };

    DetachedTask accept_loop();
    DetachedTask handle_connection(int fd);
    Async<bool> read_exact(int fd, void* buf, size_t len);
    Async<bool> write_exact(int fd, const void* buf, size_t len);

    EpollReactor reactor_;
    ThreadPool handler_pool_;
    MessageHandler handler_;
    uint32_t io_timeout_ms_;

    int server_fd_ = -1;
    std::atomic<bool> stopping_{false};
    std::atomic<size_t> active_connections_{0};
};

}  // namespace edge_orchestrator
