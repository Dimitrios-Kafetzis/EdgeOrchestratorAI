/**
 * @file bench_offload.cpp
 * @brief Cross-node offload round-trip latency, broken down by phase.
 * @author Dimitris Kafetzis
 *
 * Client and server halves of a real offload round trip over the LAN,
 * using the production wire path: OffloadCodec (Protobuf Envelope) over
 * TcpTransport, executed by TaskRunner — the same components
 * Orchestrator::offload_to_peer and handle_offload_request use.
 *
 * The wire contract is one request/response per connection, and the
 * production client (offload_to_peer) opens a fresh connection per task,
 * so the bench does the same and reports the connect cost as its own
 * phase. Per round trip:
 *   connect      TCP connect to the peer
 *   serialize    encode_request (Protobuf)
 *   wire         send + receive, minus the execution time the server
 *                reports in the response (so it includes network plus
 *                the server's decode/dispatch/encode overhead)
 *   execute      the server-reported task execution time
 *   deserialize  decode_response (Protobuf)
 *
 * Usage:
 *   bench_offload --serve <port>
 *   bench_offload --client <host> <port> [reps]
 */

#include "executor/memory_pool.hpp"
#include "executor/task_runner.hpp"
#include "network/async_transport.hpp"
#include "network/transport.hpp"
#include "orchestrator/orchestrator.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace edge_orchestrator;
using Clock = std::chrono::steady_clock;

namespace {

double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    size_t idx = std::min(static_cast<size_t>(p * static_cast<double>(v.size())),
                          v.size() - 1);
    return v[idx];
}

double mean(const std::vector<double>& v) {
    return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

int run_server(uint16_t port) {
    MemoryPool pool(64 << 20);
    TaskRunner runner;
    AsyncTransport server;

    auto listened = server.listen(port);
    if (!listened.has_value()) {
        std::cerr << "listen failed: " << listened.error().message << "\n";
        return 1;
    }

    server.serve([&](const std::vector<uint8_t>& request) -> std::vector<uint8_t> {
        std::string task_id;
        TaskProfile profile;
        std::vector<uint8_t> input;
        if (!OffloadCodec::decode_request(request, task_id, profile, input)) {
            return OffloadCodec::encode_response(false, Duration{0}, 0,
                                                 "Failed to decode request");
        }
        pool.reset();
        auto result = runner.execute(task_id, profile, pool, std::stop_token{});
        return OffloadCodec::encode_response(
            result.final_state == TaskState::Completed, result.actual_duration,
            result.peak_memory_bytes, result.error_message.value_or(""));
    });

    std::cout << "bench_offload serving on port " << port << " (Ctrl-C to stop)\n";
    while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
}

int run_client(const std::string& host, uint16_t port, size_t reps) {
    struct Case {
        const char* name;
        Duration compute;
        size_t payload_bytes;
    };
    const Case cases[] = {
        {"echo_0B", Duration{0}, 0},
        {"echo_64KB", Duration{0}, 64 * 1024},
        {"exec1ms_0B", Duration{1000}, 0},
        {"exec10ms_0B", Duration{10000}, 0},
        {"exec10ms_64KB", Duration{10000}, 64 * 1024},
    };

    std::cout << "case,reps,connect_us_mean,serialize_us_mean,wire_us_mean,"
                 "execute_us_mean,deserialize_us_mean,total_us_mean,"
                 "total_us_p99,request_bytes,failures\n";

    for (const auto& c : cases) {
        TaskProfile profile{.compute_cost = c.compute,
                            .memory_bytes = 1 << 20,
                            .input_bytes = c.payload_bytes,
                            .output_bytes = 0};
        std::vector<uint8_t> payload(c.payload_bytes, 0xAB);

        std::vector<double> t_conn, t_ser, t_wire, t_exec, t_deser, t_total;
        size_t request_bytes = 0;

        auto round_trip = [&](const std::string& task_id,
                              bool record) -> bool {
            auto t0 = Clock::now();
            TcpTransport client;
            auto conn = client.connect(host, port, 5000);
            if (!conn.has_value()) {
                std::cerr << "connect failed: " << conn.error().message << "\n";
                return false;
            }
            auto t1 = Clock::now();

            auto request = OffloadCodec::encode_request(task_id, profile, payload);
            auto t2 = Clock::now();

            auto sent = client.send(request);
            if (!sent.has_value()) { std::cerr << "send failed\n"; return false; }
            auto reply = client.receive(10000);
            if (!reply.has_value()) { std::cerr << "receive failed\n"; return false; }
            auto t3 = Clock::now();

            bool success = false;
            Duration exec_duration{0};
            uint64_t peak_memory = 0;
            std::string error;
            std::vector<uint8_t> output;
            if (!OffloadCodec::decode_response(*reply, success, exec_duration,
                                               peak_memory, error, output) ||
                !success) {
                std::cerr << "bad response: " << error << "\n";
                return false;
            }
            auto t4 = Clock::now();
            client.disconnect();

            if (record) {
                auto us = [](auto a, auto b) {
                    return std::chrono::duration<double, std::micro>(b - a).count();
                };
                double exec_us = static_cast<double>(exec_duration.count());
                t_conn.push_back(us(t0, t1));
                t_ser.push_back(us(t1, t2));
                t_wire.push_back(us(t2, t3) - exec_us);
                t_exec.push_back(exec_us);
                t_deser.push_back(us(t3, t4));
                t_total.push_back(us(t0, t4));
                request_bytes = request.size();
            }
            return true;
        };

        // A lossy link can drop an individual round trip; that is data,
        // not a reason to abort the case. Failed attempts are excluded
        // from the timing stats and reported in their own column.
        size_t failures = 0;
        for (size_t i = 0; i < 5; ++i) {
            if (!round_trip("warmup", false)) ++failures;
        }
        for (size_t i = 0; i < reps; ++i) {
            if (!round_trip("bench_" + std::to_string(i), true)) ++failures;
            if (failures > reps / 2) {
                std::cerr << "aborting case " << c.name
                          << ": too many failures\n";
                return 1;
            }
        }
        if (t_total.empty()) return 1;

        std::cout << c.name << "," << t_total.size() << "," << std::fixed
                  << std::setprecision(1) << mean(t_conn) << "," << mean(t_ser)
                  << "," << mean(t_wire) << "," << mean(t_exec) << ","
                  << mean(t_deser) << "," << mean(t_total) << ","
                  << percentile(t_total, 0.99) << "," << request_bytes << ","
                  << failures << "\n";
    }
    return 0;
}

}  // anonymous namespace

int main(int argc, char* argv[]) {
    if (argc >= 3 && std::strcmp(argv[1], "--serve") == 0) {
        return run_server(static_cast<uint16_t>(std::stoi(argv[2])));
    }
    if (argc >= 4 && std::strcmp(argv[1], "--client") == 0) {
        size_t reps = (argc > 4) ? static_cast<size_t>(std::stoul(argv[4])) : 200;
        return run_client(argv[2], static_cast<uint16_t>(std::stoi(argv[3])), reps);
    }
    std::cerr << "Usage: bench_offload --serve <port>\n"
              << "       bench_offload --client <host> <port> [reps]\n";
    return 1;
}
