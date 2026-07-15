/**
 * @file reactor.cpp
 * @brief EpollReactor implementation.
 * @author Dimitris Kafetzis
 *
 * One thread, one epoll instance, one parked coroutine per fd. The
 * model is deliberately narrower than a general-purpose event loop:
 *
 * - Registrations use EPOLLONESHOT, so an fd is armed for exactly one
 *   readiness event and then removed. The awaiting coroutine re-arms by
 *   awaiting again. This sidesteps the classic epoll hazards (stale
 *   events after close, spurious wakeups resuming a dead handle) at the
 *   cost of one epoll_ctl per await — negligible next to the socket I/O
 *   it guards.
 * - Timeouts are per-waiter deadlines, not epoll timers. The loop's
 *   epoll_wait timeout is simply the nearest deadline (capped at 500 ms
 *   so stop requests stay responsive), and an eventfd wakes the loop
 *   early whenever a new waiter registers with a nearer deadline.
 * - A waiter resumes in exactly one of three ways, and always exactly
 *   once: readiness (ready_ = true), deadline expiry (ready_ = false),
 *   or reactor shutdown (ready_ = false, via the drain loop below).
 *   Callers can't tell timeout from shutdown apart — both mean "your
 *   I/O did not happen", which is all a connection handler needs.
 */

#include "network/reactor.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <vector>

namespace edge_orchestrator {

// ─────────────────────────────────────────────
// FdAwaiter
// ─────────────────────────────────────────────

bool FdAwaiter::await_suspend(std::coroutine_handle<> handle) {
    // false = do not suspend: the coroutine continues immediately and
    // await_resume reports ready_ == false (shutdown path).
    return reactor_.register_waiter(this, handle);
}

// ─────────────────────────────────────────────
// EpollReactor
// ─────────────────────────────────────────────

EpollReactor::EpollReactor() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    wake_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

    if (epoll_fd_ >= 0 && wake_fd_ >= 0) {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = wake_fd_;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev);
    }
}

EpollReactor::~EpollReactor() {
    stop();
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
    if (wake_fd_ >= 0) ::close(wake_fd_);
}

void EpollReactor::start() {
    if (running_ || epoll_fd_ < 0) return;
    running_ = true;
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

void EpollReactor::stop() {
    if (!running_) return;
    thread_.request_stop();
    wake();
    thread_.join();
    running_ = false;
}

FdAwaiter EpollReactor::wait_readable(int fd, uint32_t timeout_ms) {
    return FdAwaiter{*this, fd, EPOLLIN, timeout_ms};
}

FdAwaiter EpollReactor::wait_writable(int fd, uint32_t timeout_ms) {
    return FdAwaiter{*this, fd, EPOLLOUT, timeout_ms};
}

/**
 * @brief Park a coroutine until its fd is ready or its deadline passes.
 *
 * Called from await_suspend, i.e. on whatever thread the coroutine was
 * running on — not necessarily the reactor thread, hence the lock.
 * Returns false (don't suspend) when the reactor is shutting down or
 * epoll rejects the fd; the coroutine then continues inline and sees
 * ready_ == false.
 */
bool EpollReactor::register_waiter(FdAwaiter* awaiter,
                                   std::coroutine_handle<> handle) {
    {
        std::lock_guard lock(mutex_);
        if (!running_ || thread_.get_stop_token().stop_requested()) {
            awaiter->ready_ = false;
            return false;
        }

        waiters_[awaiter->fd_] = Waiter{
            .awaiter = awaiter,
            .handle = handle,
            .deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(awaiter->timeout_ms_),
        };

        epoll_event ev{};
        ev.events = awaiter->events_ | EPOLLONESHOT;
        ev.data.fd = awaiter->fd_;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, awaiter->fd_, &ev) < 0) {
            waiters_.erase(awaiter->fd_);
            awaiter->ready_ = false;
            return false;
        }
    }
    wake();  // recompute the epoll_wait deadline
    return true;
}

void EpollReactor::wake() {
    if (wake_fd_ >= 0) {
        uint64_t one = 1;
        [[maybe_unused]] auto n = ::write(wake_fd_, &one, sizeof(one));
    }
}

/**
 * @brief The reactor loop: wait, resume ready waiters, expire deadlines.
 *
 * Resumptions happen outside the lock because a resumed coroutine
 * usually runs straight to its next co_await, which re-enters
 * register_waiter on this same thread — holding the lock across the
 * resume would deadlock immediately.
 */
void EpollReactor::run(std::stop_token stop) {
    constexpr int MAX_EVENTS = 32;
    epoll_event events[MAX_EVENTS];

    while (!stop.stop_requested()) {
        // Timeout = nearest waiter deadline (bounded so stop stays responsive).
        int timeout_ms = 500;
        {
            std::lock_guard lock(mutex_);
            auto now = std::chrono::steady_clock::now();
            for (const auto& [fd, waiter] : waiters_) {
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    waiter.deadline - now).count();
                timeout_ms = std::min(timeout_ms,
                                      static_cast<int>(std::max<int64_t>(0, remaining)));
            }
        }

        int n = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, timeout_ms);

        std::vector<std::pair<FdAwaiter*, std::coroutine_handle<>>> to_resume;
        {
            std::lock_guard lock(mutex_);

            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;
                if (fd == wake_fd_) {
                    uint64_t drained;
                    [[maybe_unused]] auto r = ::read(wake_fd_, &drained, sizeof(drained));
                    continue;
                }
                auto it = waiters_.find(fd);
                if (it == waiters_.end()) continue;
                it->second.awaiter->ready_ = true;
                to_resume.emplace_back(it->second.awaiter, it->second.handle);
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
                waiters_.erase(it);
            }

            // Expired deadlines resume with ready_ == false.
            auto now = std::chrono::steady_clock::now();
            for (auto it = waiters_.begin(); it != waiters_.end(); ) {
                if (it->second.deadline <= now) {
                    it->second.awaiter->ready_ = false;
                    to_resume.emplace_back(it->second.awaiter, it->second.handle);
                    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, it->first, nullptr);
                    it = waiters_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Resume outside the lock: a resumed coroutine typically awaits
        // again, which re-enters register_waiter.
        for (auto& [awaiter, handle] : to_resume) {
            handle.resume();
        }
    }

    // Drain: every parked coroutine resumes with ready_ == false so
    // connection handlers can fail cleanly before the reactor exits.
    // register_waiter refuses new parks once stop is requested, so this
    // terminates.
    for (;;) {
        std::vector<std::pair<FdAwaiter*, std::coroutine_handle<>>> to_resume;
        {
            std::lock_guard lock(mutex_);
            for (auto& [fd, waiter] : waiters_) {
                waiter.awaiter->ready_ = false;
                to_resume.emplace_back(waiter.awaiter, waiter.handle);
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
            }
            waiters_.clear();
        }
        if (to_resume.empty()) break;
        for (auto& [awaiter, handle] : to_resume) {
            handle.resume();
        }
    }
}

}  // namespace edge_orchestrator
