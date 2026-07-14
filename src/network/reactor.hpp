/**
 * @file reactor.hpp
 * @brief Single-threaded epoll reactor for coroutine-based async I/O.
 * @author Dimitris Kafetzis
 *
 * Coroutines co_await wait_readable()/wait_writable(); the reactor parks
 * them until the fd is ready or the deadline passes, then resumes them on
 * the reactor thread. One waiter per fd at a time (the transport's
 * one-coroutine-per-connection model), registered EPOLLONESHOT.
 *
 * epoll is used here (rather than poll(), which the synchronous
 * TcpTransport keeps) because the async server multiplexes many
 * connections on one thread — the O(ready) wakeup is what pays.
 */

#pragma once

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace edge_orchestrator {

class EpollReactor;

/**
 * @brief Awaitable for fd readiness. co_await yields true when the fd is
 *        ready, false on timeout or reactor shutdown.
 */
class FdAwaiter {
public:
    FdAwaiter(EpollReactor& reactor, int fd, uint32_t events, uint32_t timeout_ms)
        : reactor_(reactor), fd_(fd), events_(events), timeout_ms_(timeout_ms) {}

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle);
    bool await_resume() const noexcept { return ready_; }

private:
    friend class EpollReactor;
    EpollReactor& reactor_;
    int fd_;
    uint32_t events_;
    uint32_t timeout_ms_;
    bool ready_ = false;
};

class EpollReactor {
public:
    EpollReactor();
    ~EpollReactor();

    EpollReactor(const EpollReactor&) = delete;
    EpollReactor& operator=(const EpollReactor&) = delete;

    void start();
    void stop();   ///< Drains pending waiters (resumed with false), then joins.

    [[nodiscard]] FdAwaiter wait_readable(int fd, uint32_t timeout_ms);
    [[nodiscard]] FdAwaiter wait_writable(int fd, uint32_t timeout_ms);

    [[nodiscard]] bool running() const noexcept { return running_; }

private:
    friend class FdAwaiter;

    struct Waiter {
        FdAwaiter* awaiter = nullptr;
        std::coroutine_handle<> handle;
        std::chrono::steady_clock::time_point deadline;
    };

    /// Returns false (and does not park) if the reactor is shutting down.
    bool register_waiter(FdAwaiter* awaiter, std::coroutine_handle<> handle);
    void wake();
    void run(std::stop_token stop);

    int epoll_fd_ = -1;
    int wake_fd_ = -1;    ///< eventfd used to interrupt epoll_wait
    bool running_ = false;

    std::mutex mutex_;
    std::unordered_map<int, Waiter> waiters_;   // keyed by fd

    std::jthread thread_;
};

}  // namespace edge_orchestrator
