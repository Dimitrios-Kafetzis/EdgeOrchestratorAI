/**
 * @file transport.cpp
 * @brief TcpTransport implementation — length-prefixed TCP messaging.
 * @author Dimitris Kafetzis
 *
 * Wire format: [uint32_t big-endian length][payload bytes]
 * Uses poll() for non-blocking I/O with timeouts.
 */

#include "network/transport.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace edge_orchestrator {

namespace {

/**
 * @brief Set TCP_NODELAY and keepalive on a connected socket.
 */
void configure_socket(int fd) {
    int flag = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
}

/**
 * @brief Encode a uint32_t in big-endian (network byte order).
 */
void encode_u32(uint8_t* buf, uint32_t val) {
    buf[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    buf[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    buf[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    buf[3] = static_cast<uint8_t>(val & 0xFF);
}

/**
 * @brief Decode a uint32_t from big-endian.
 */
uint32_t decode_u32(const uint8_t* buf) {
    return (static_cast<uint32_t>(buf[0]) << 24)
         | (static_cast<uint32_t>(buf[1]) << 16)
         | (static_cast<uint32_t>(buf[2]) << 8)
         | static_cast<uint32_t>(buf[3]);
}

}  // anonymous namespace

// ─────────────────────────────────────────────
// Construction / Destruction
// ─────────────────────────────────────────────

TcpTransport::TcpTransport() = default;

TcpTransport::~TcpTransport() {
    stop_serving();
    disconnect();
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
}

// ─────────────────────────────────────────────
// Client Side
// ─────────────────────────────────────────────

Result<void> TcpTransport::connect(const std::string& address, uint16_t port,
                                    uint32_t timeout_ms) {
    if (client_fd_ >= 0) {
        return Error{"Already connected"};
    }

    client_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (client_fd_ < 0) {
        return Error{"Failed to create socket: " + std::string(strerror(errno))};
    }

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(port);

    if (::inet_pton(AF_INET, address.c_str(), &server.sin_addr) != 1) {
        ::close(client_fd_);
        client_fd_ = -1;
        return Error{"Invalid address: " + address};
    }

    int ret = ::connect(client_fd_, reinterpret_cast<const sockaddr*>(&server), sizeof(server));
    if (ret < 0 && errno != EINPROGRESS) {
        ::close(client_fd_);
        client_fd_ = -1;
        return Error{"Connect failed: " + std::string(strerror(errno))};
    }

    if (ret < 0) {
        // Wait for connection to complete
        pollfd pfd{};
        pfd.fd = client_fd_;
        pfd.events = POLLOUT;

        int ready = ::poll(&pfd, 1, static_cast<int>(timeout_ms));
        if (ready <= 0) {
            ::close(client_fd_);
            client_fd_ = -1;
            return Error{"Connect timed out"};
        }

        // Check for connection errors
        int err = 0;
        socklen_t len = sizeof(err);
        ::getsockopt(client_fd_, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            ::close(client_fd_);
            client_fd_ = -1;
            return Error{"Connect failed: " + std::string(strerror(err))};
        }
    }

    configure_socket(client_fd_);
    return Result<void>{};
}

Result<void> TcpTransport::send(const std::vector<uint8_t>& data) {
    if (client_fd_ < 0) {
        return Error{"Not connected"};
    }
    return send_on_fd(client_fd_, data);
}

Result<std::vector<uint8_t>> TcpTransport::receive(uint32_t timeout_ms) {
    if (client_fd_ < 0) {
        return Error{"Not connected"};
    }
    return recv_on_fd(client_fd_, timeout_ms);
}

void TcpTransport::disconnect() {
    if (client_fd_ >= 0) {
        ::shutdown(client_fd_, SHUT_RDWR);
        ::close(client_fd_);
        client_fd_ = -1;
    }
}

// ─────────────────────────────────────────────
// Server Side
// ─────────────────────────────────────────────

Result<void> TcpTransport::listen(uint16_t port, int backlog) {
    if (server_fd_ >= 0) {
        return Error{"Already listening"};
    }

    server_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
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

void TcpTransport::serve(MessageHandler handler) {
    if (server_fd_ < 0) return;

    serving_ = true;
    serve_thread_ = std::jthread([this, handler = std::move(handler)](std::stop_token stop) {
        while (!stop.stop_requested() && serving_) {
            pollfd pfd{};
            pfd.fd = server_fd_;
            pfd.events = POLLIN;

            int ready = ::poll(&pfd, 1, 100);  // 100ms timeout for stop check
            if (ready <= 0) continue;

            sockaddr_in client_addr{};
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = ::accept(server_fd_,
                reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

            if (client_fd < 0) continue;

            configure_socket(client_fd);

            // Handle the connection (single-threaded for simplicity;
            // could be extended to a thread pool)
            auto request = recv_on_fd(client_fd, 10000);
            if (request.has_value()) {
                auto response = handler(*request);
                send_on_fd(client_fd, response);
            }

            ::shutdown(client_fd, SHUT_RDWR);
            ::close(client_fd);
        }
    });
}

void TcpTransport::stop_serving() {
    serving_ = false;
    if (serve_thread_.joinable()) {
        serve_thread_.request_stop();
    }
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
}

// ─────────────────────────────────────────────
// State Queries
// ─────────────────────────────────────────────

bool TcpTransport::is_connected() const noexcept {
    return client_fd_ >= 0;
}

bool TcpTransport::is_listening() const noexcept {
    return server_fd_ >= 0;
}

// ─────────────────────────────────────────────
// Wire Protocol Helpers
// ─────────────────────────────────────────────

Result<void> TcpTransport::send_on_fd(int fd, const std::vector<uint8_t>& data) {
    if (data.size() > MAX_MESSAGE_SIZE) {
        return Error{"Message too large"};
    }

    // Send length header
    uint8_t header[4];
    encode_u32(header, static_cast<uint32_t>(data.size()));

    if (!send_all(fd, header, 4)) {
        return Error{"Failed to send header"};
    }

    // Send payload
    if (!data.empty()) {
        if (!send_all(fd, data.data(), data.size())) {
            return Error{"Failed to send payload"};
        }
    }

    return Result<void>{};
}

Result<std::vector<uint8_t>> TcpTransport::recv_on_fd(int fd, uint32_t timeout_ms) {
    // Read length header
    uint8_t header[4];
    if (!recv_all(fd, header, 4, timeout_ms)) {
        return Error{"Failed to receive header"};
    }

    uint32_t length = decode_u32(header);
    if (length > MAX_MESSAGE_SIZE) {
        return Error{"Message too large: " + std::to_string(length) + " bytes"};
    }

    // Read payload
    std::vector<uint8_t> payload(length);
    if (length > 0) {
        if (!recv_all(fd, payload.data(), length, timeout_ms)) {
            return Error{"Failed to receive payload"};
        }
    }

    return payload;
}

bool TcpTransport::send_all(int fd, const void* buf, size_t len, uint32_t timeout_ms) {
    const auto* ptr = static_cast<const uint8_t*>(buf);
    size_t remaining = len;

    while (remaining > 0) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;

        int ready = ::poll(&pfd, 1, static_cast<int>(timeout_ms));
        if (ready <= 0) return false;

        auto sent = ::send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (sent <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }

        ptr += sent;
        remaining -= static_cast<size_t>(sent);
    }

    return true;
}

bool TcpTransport::recv_all(int fd, void* buf, size_t len, uint32_t timeout_ms) {
    auto* ptr = static_cast<uint8_t*>(buf);
    size_t remaining = len;

    while (remaining > 0) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;

        int ready = ::poll(&pfd, 1, static_cast<int>(timeout_ms));
        if (ready <= 0) return false;

        auto received = ::recv(fd, ptr, remaining, 0);
        if (received <= 0) {
            if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            return false;  // Connection closed or error
        }

        ptr += received;
        remaining -= static_cast<size_t>(received);
    }

    return true;
}

}  // namespace edge_orchestrator
