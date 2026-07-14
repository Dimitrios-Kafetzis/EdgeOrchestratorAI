/**
 * @file offload_codec.cpp
 * @brief OffloadCodec — Protobuf serialization for task offloading.
 * @author Dimitris Kafetzis
 *
 * Wire format: a length-prefixed frame (TcpTransport) whose payload is a
 * serialized edge_orchestrator.Envelope (see proto/protocol.proto). The
 * Envelope's oneof discriminates request from response, so the server can
 * dispatch without out-of-band typing.
 *
 * Protobuf was chosen over the previous hand-rolled binary format for
 * schema evolution (fields can be added without breaking deployed nodes)
 * and cross-language interop (a Go or Python peer can speak this wire
 * format from the same .proto). The UDP discovery path deliberately stays
 * a fixed 72-byte packed struct — it is high-frequency, latency-tolerant
 * of loss, and never evolves independently per field.
 *
 * request_id is left empty for now: correlation is implicit in the
 * one-request-per-connection transport. The field exists in the schema so
 * pipelined transports can adopt it without a wire break.
 */

#include "orchestrator/orchestrator.hpp"

#include "protocol.pb.h"

namespace edge_orchestrator {

// ─────────────────────────────────────────────
// Request
// ─────────────────────────────────────────────

std::vector<uint8_t> OffloadCodec::encode_request(
    const std::string& task_id,
    const TaskProfile& profile,
    const std::vector<uint8_t>& input_data) {

    Envelope env;
    auto* req = env.mutable_offload_request();
    req->set_task_id(task_id);

    auto* prof = req->mutable_profile();
    prof->set_compute_cost_us(static_cast<uint64_t>(profile.compute_cost.count()));
    prof->set_memory_bytes(profile.memory_bytes);
    prof->set_input_bytes(profile.input_bytes);
    prof->set_output_bytes(profile.output_bytes);

    if (!input_data.empty()) {
        req->set_input_data(input_data.data(), input_data.size());
    }

    std::vector<uint8_t> buf(env.ByteSizeLong());
    env.SerializeToArray(buf.data(), static_cast<int>(buf.size()));
    return buf;
}

bool OffloadCodec::decode_request(
    const std::vector<uint8_t>& data,
    std::string& task_id,
    TaskProfile& profile,
    std::vector<uint8_t>& input_data) {

    Envelope env;
    if (!env.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
        return false;
    }
    if (!env.has_offload_request()) {
        return false;
    }

    const auto& req = env.offload_request();
    task_id = req.task_id();

    const auto& prof = req.profile();
    profile.compute_cost = Duration{static_cast<int64_t>(prof.compute_cost_us())};
    profile.memory_bytes = prof.memory_bytes();
    profile.input_bytes = prof.input_bytes();
    profile.output_bytes = prof.output_bytes();

    const auto& input = req.input_data();
    input_data.assign(input.begin(), input.end());

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

    Envelope env;
    auto* resp = env.mutable_offload_response();
    resp->set_success(success);
    resp->set_actual_duration_us(static_cast<uint64_t>(duration.count()));
    resp->set_peak_memory_bytes(peak_memory);
    resp->set_error_message(error_msg);

    if (!output.empty()) {
        resp->set_output_data(output.data(), output.size());
    }

    std::vector<uint8_t> buf(env.ByteSizeLong());
    env.SerializeToArray(buf.data(), static_cast<int>(buf.size()));
    return buf;
}

bool OffloadCodec::decode_response(
    const std::vector<uint8_t>& data,
    bool& success,
    Duration& duration,
    uint64_t& peak_memory,
    std::string& error_msg,
    std::vector<uint8_t>& output) {

    Envelope env;
    if (!env.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
        return false;
    }
    if (!env.has_offload_response()) {
        return false;
    }

    const auto& resp = env.offload_response();
    success = resp.success();
    duration = Duration{static_cast<int64_t>(resp.actual_duration_us())};
    peak_memory = resp.peak_memory_bytes();
    error_msg = resp.error_message();

    const auto& out = resp.output_data();
    output.assign(out.begin(), out.end());

    return true;
}

}  // namespace edge_orchestrator
