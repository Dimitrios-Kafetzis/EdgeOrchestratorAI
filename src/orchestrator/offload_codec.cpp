/**
 * @file offload_codec.cpp
 * @brief OffloadCodec binary serialization for task offloading.
 * @author Dimitris Kafetzis
 *
 * Wire format (all multi-byte values are big-endian):
 *
 * Request:
 *   [4B task_id_len][task_id bytes][8B compute_cost_us][8B memory_bytes]
 *   [8B input_bytes][8B output_bytes][input_data...]
 *
 * Response:
 *   [1B status: 0=success, 1=error]
 *   [8B duration_us][8B peak_memory_bytes]
 *   [4B error_msg_len][error_msg bytes]
 *   [output_data...]
 */

#include "orchestrator/orchestrator.hpp"

#include <cstring>

namespace edge_orchestrator {

// ─────────────────────────────────────────────
// Helper: big-endian encode/decode
// ─────────────────────────────────────────────

void OffloadCodec::put_u64(std::vector<uint8_t>& buf, uint64_t val) {
    for (int i = 7; i >= 0; --i) {
        buf.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    }
}

void OffloadCodec::put_u32(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
}

uint64_t OffloadCodec::get_u64(const uint8_t* p) {
    uint64_t val = 0;
    for (int i = 0; i < 8; ++i) {
        val = (val << 8) | p[i];
    }
    return val;
}

uint32_t OffloadCodec::get_u32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24)
         | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8)
         | static_cast<uint32_t>(p[3]);
}

// ─────────────────────────────────────────────
// Request
// ─────────────────────────────────────────────

std::vector<uint8_t> OffloadCodec::encode_request(
    const std::string& task_id,
    const TaskProfile& profile,
    const std::vector<uint8_t>& input_data) {

    std::vector<uint8_t> buf;
    buf.reserve(4 + task_id.size() + 32 + input_data.size());

    // Task ID
    put_u32(buf, static_cast<uint32_t>(task_id.size()));
    buf.insert(buf.end(), task_id.begin(), task_id.end());

    // Profile
    put_u64(buf, static_cast<uint64_t>(profile.compute_cost.count()));
    put_u64(buf, profile.memory_bytes);
    put_u64(buf, profile.input_bytes);
    put_u64(buf, profile.output_bytes);

    // Input data
    buf.insert(buf.end(), input_data.begin(), input_data.end());

    return buf;
}

bool OffloadCodec::decode_request(
    const std::vector<uint8_t>& data,
    std::string& task_id,
    TaskProfile& profile,
    std::vector<uint8_t>& input_data) {

    const size_t MIN_SIZE = 4 + 32;  // task_id_len + 4 × uint64
    if (data.size() < MIN_SIZE) return false;

    const uint8_t* p = data.data();
    size_t offset = 0;

    // Task ID
    uint32_t id_len = get_u32(p + offset);
    offset += 4;
    if (offset + id_len + 32 > data.size()) return false;
    task_id.assign(reinterpret_cast<const char*>(p + offset), id_len);
    offset += id_len;

    // Profile
    profile.compute_cost = Duration{static_cast<int64_t>(get_u64(p + offset))};
    offset += 8;
    profile.memory_bytes = get_u64(p + offset);
    offset += 8;
    profile.input_bytes = get_u64(p + offset);
    offset += 8;
    profile.output_bytes = get_u64(p + offset);
    offset += 8;

    // Input data (remaining bytes)
    if (offset < data.size()) {
        input_data.assign(p + offset, p + data.size());
    } else {
        input_data.clear();
    }

    return true;
}

// ─────────────────────────────────────────────
// Response
// ─────────────────────────────────────────────

std::vector<uint8_t> OffloadCodec::encode_response(
    bool success,
    Duration duration,
    uint64_t peak_memory,
    const std::string& error_msg,
    const std::vector<uint8_t>& output) {

    std::vector<uint8_t> buf;
    buf.reserve(1 + 16 + 4 + error_msg.size() + output.size());

    // Status
    buf.push_back(success ? 0x00 : 0x01);

    // Duration and peak memory
    put_u64(buf, static_cast<uint64_t>(duration.count()));
    put_u64(buf, peak_memory);

    // Error message
    put_u32(buf, static_cast<uint32_t>(error_msg.size()));
    buf.insert(buf.end(), error_msg.begin(), error_msg.end());

    // Output data
    buf.insert(buf.end(), output.begin(), output.end());

    return buf;
}

bool OffloadCodec::decode_response(
    const std::vector<uint8_t>& data,
    bool& success,
    Duration& duration,
    uint64_t& peak_memory,
    std::string& error_msg,
    std::vector<uint8_t>& output) {

    const size_t MIN_SIZE = 1 + 16 + 4;  // status + duration + peak_mem + err_len
    if (data.size() < MIN_SIZE) return false;

    const uint8_t* p = data.data();
    size_t offset = 0;

    // Status
    success = (p[offset] == 0x00);
    offset += 1;

    // Duration
    duration = Duration{static_cast<int64_t>(get_u64(p + offset))};
    offset += 8;

    // Peak memory
    peak_memory = get_u64(p + offset);
    offset += 8;

    // Error message
    uint32_t err_len = get_u32(p + offset);
    offset += 4;
    if (offset + err_len > data.size()) return false;
    error_msg.assign(reinterpret_cast<const char*>(p + offset), err_len);
    offset += err_len;

    // Output data
    if (offset < data.size()) {
        output.assign(p + offset, p + data.size());
    } else {
        output.clear();
    }

    return true;
}

}  // namespace edge_orchestrator
