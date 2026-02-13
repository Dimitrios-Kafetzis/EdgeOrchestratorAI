/**
 * @file transport.hpp
 * @brief TCP transport for task offloading with length-prefixed framing.
 * @author Dimitris Kafetzis
 *
 * Provides both client (connect + send/receive) and server (accept + handle)
 * sides. Messages are framed as [4-byte big-endian length][payload].
 * Uses non-blocking I/O with poll() for cooperative scheduling.
 */

#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace edge_orchestrator {

/**
 * @brief Length-prefixed TCP transport for inter-node communication.
 *
 * Wire format per message:
 *   [uint32_t big-endian length][payload bytes]
 *
 * Maximum message size: 16 MB (configurable).
 */
class TcpTransport {
public:
    static constexpr uint32_t MAX_MESSAGE_SIZE = 16 * 1024 * 1024;  // 16 MB
    static constexpr int DEFAULT_BACKLOG = 16;

    TcpTransport();
    ~TcpTransport();

    // Non-copyable
    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    // ── Client-side ──────────────────────────
    Result<void> connect(const std::string& address, uint16_t port,
                         uint32_t timeout_ms = 5000);
    Result<void> send(const std::vector<uint8_t>& data);
    Result<std::vector<uint8_t>> receive(uint32_t timeout_ms = 10000);
    void disconnect();

    // ── Server-side ──────────────────────────
    using MessageHandler = std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)>;

    Result<void> listen(uint16_t port, int backlog = DEFAULT_BACKLOG);
    void serve(MessageHandler handler);
    void stop_serving();

    // ── State queries ────────────────────────
    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] bool is_listening() const noexcept;

private:
    // Wire helpers
    Result<void> send_on_fd(int fd, const std::vector<uint8_t>& data);
    Result<std::vector<uint8_t>> recv_on_fd(int fd, uint32_t timeout_ms);
    static bool send_all(int fd, const void* buf, size_t len, uint32_t timeout_ms = 5000);
    static bool recv_all(int fd, void* buf, size_t len, uint32_t timeout_ms = 10000);

    int client_fd_ = -1;
    int server_fd_ = -1;
    std::jthread serve_thread_;
    bool serving_ = false;
};

}  // namespace edge_orchestrator
