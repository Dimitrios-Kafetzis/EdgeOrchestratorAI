/**
 * @file async_transport.cpp
 * @brief AsyncTransport implementation.
 * @author Dimitris Kafetzis
 */

#include "network/async_transport.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <thread>

namespace edge_orchestrator {

namespace {

void encode_u32_be(uint8_t* buf, uint32_t val) {
    buf[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    buf[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    buf[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    buf[3] = static_cast<uint8_t>(val & 0xFF);
}

uint32_t decode_u32_be(const uint8_t* buf) {
    return (static_cast<uint32_t>(buf[0]) << 24)
         | (static_cast<uint32_t>(buf[1]) << 16)
         | (static_cast<uint32_t>(buf[2]) << 8)
         | static_cast<uint32_t>(buf[3]);
}

}  // anonymous namespace

AsyncTransport::AsyncTransport(size_t handler_threads, uint32_t io_timeout_ms)
    : handler_pool_(handler_threads), io_timeout_ms_(io_timeout_ms) {}

AsyncTransport::~AsyncTransport() {
    stop_serving();
}

Result<void> AsyncTransport::listen(uint16_t port, int backlog) {
    if (server_fd_ >= 0) {
        return Error{"Already listening"};
    }

    server_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server_fd_ < 0) {
        return Error{"Failed to create server socket: " + std::string(strerror(errno))};
    }

    int optval = 1;
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(server_fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        return Error{"Bind failed: " + std::string(strerror(errno))};
    }

    if (::listen(server_fd_, backlog) < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        return Error{"Listen failed: " + std::string(strerror(errno))};
    }

    return Result<void>{};
}

void AsyncTransport::serve(MessageHandler handler) {
    if (server_fd_ < 0 || reactor_.running()) return;
    handler_ = std::move(handler);
    stopping_ = false;
    reactor_.start();
    accept_loop();
}

void AsyncTransport::stop_serving() {
    if (stopping_.exchange(true)) return;

    // In-flight handlers finish on the pool; their next socket await
    // fails fast because the reactor refuses new parks once stopping.
    reactor_.stop();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (active_connections_.load() > 0
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
}

DetachedTask AsyncTransport::accept_loop() {
    while (!stopping_) {
        bool ready = co_await reactor_.wait_readable(server_fd_, 500);
        if (stopping_) break;
        if (!ready) continue;  // periodic timeout keeps the loop responsive

        for (;;) {
            int client = ::accept4(server_fd_, nullptr, nullptr,
                                   SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (client < 0) break;

            int flag = 1;
            ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

            handle_connection(client);
        }
    }
}

DetachedTask AsyncTransport::handle_connection(int fd) {
    active_connections_.fetch_add(1);

    uint8_t header[4];
    if (co_await read_exact(fd, header, sizeof(header))) {
        uint32_t length = decode_u32_be(header);
        if (length <= MAX_MESSAGE_SIZE) {
            std::vector<uint8_t> payload(length);
            bool got_payload = (length == 0)
                || co_await read_exact(fd, payload.data(), payload.size());
            if (got_payload && !stopping_) {
                co_await PoolHop{handler_pool_};   // handler off the reactor
                auto response = handler_(payload);

                uint8_t resp_header[4];
                encode_u32_be(resp_header, static_cast<uint32_t>(response.size()));
                if (co_await write_exact(fd, resp_header, sizeof(resp_header))) {
                    co_await write_exact(fd, response.data(), response.size());
                }
            }
        }
        // Oversized length: close without a response, same as TcpTransport.
    }

    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    active_connections_.fetch_sub(1);
}

Async<bool> AsyncTransport::read_exact(int fd, void* buf, size_t len) {
    size_t done = 0;
    auto* p = static_cast<uint8_t*>(buf);
    while (done < len) {
        ssize_t n = ::recv(fd, p + done, len - done, 0);
        if (n > 0) {
            done += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) co_return false;  // peer closed
        if (errno != EAGAIN && errno != EWOULDBLOCK) co_return false;

        bool ready = co_await reactor_.wait_readable(fd, io_timeout_ms_);
        if (!ready) co_return false;  // timeout or shutdown
    }
    co_return true;
}

Async<bool> AsyncTransport::write_exact(int fd, const void* buf, size_t len) {
    size_t done = 0;
    const auto* p = static_cast<const uint8_t*>(buf);
    while (done < len) {
        ssize_t n = ::send(fd, p + done, len - done, MSG_NOSIGNAL);
        if (n > 0) {
            done += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) co_return false;
        if (errno != EAGAIN && errno != EWOULDBLOCK) co_return false;

        bool ready = co_await reactor_.wait_writable(fd, io_timeout_ms_);
        if (!ready) co_return false;
    }
    co_return true;
}

}  // namespace edge_orchestrator
