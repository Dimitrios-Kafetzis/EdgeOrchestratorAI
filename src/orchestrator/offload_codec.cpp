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
// Dispatch
// ─────────────────────────────────────────────

OffloadCodec::WireKind OffloadCodec::classify(const std::vector<uint8_t>& data) {
    Envelope env;
    if (!env.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
        return WireKind::Unknown;
    }
    switch (env.payload_case()) {
        case Envelope::kOffloadRequest:     return WireKind::OffloadRequest;
        case Envelope::kOffloadResponse:    return WireKind::OffloadResponse;
        case Envelope::kWorkloadSubmission: return WireKind::WorkloadSubmission;
        case Envelope::kWorkloadResult:     return WireKind::WorkloadResult;
        default:                            return WireKind::Unknown;
    }
}

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

// ─────────────────────────────────────────────
// Workload Submission
// ─────────────────────────────────────────────

std::vector<uint8_t> OffloadCodec::encode_submission(
    const std::string& workload_id,
    const WorkloadDAG& dag) {

    Envelope env;
    auto* sub = env.mutable_workload_submission();
    sub->set_workload_id(workload_id);

    for (const auto& task_id : dag.topological_order()) {
        auto task = dag.get_task(task_id);
        if (!task) continue;

        auto* spec = sub->add_tasks();
        spec->set_task_id(task->id);
        spec->set_name(task->name);

        auto* prof = spec->mutable_profile();
        prof->set_compute_cost_us(static_cast<uint64_t>(task->profile.compute_cost.count()));
        prof->set_memory_bytes(task->profile.memory_bytes);
        prof->set_input_bytes(task->profile.input_bytes);
        prof->set_output_bytes(task->profile.output_bytes);

        for (const auto& dep : task->dependencies) {
            spec->add_dependencies(dep);
        }
    }

    std::vector<uint8_t> buf(env.ByteSizeLong());
    env.SerializeToArray(buf.data(), static_cast<int>(buf.size()));
    return buf;
}

bool OffloadCodec::decode_submission(
    const std::vector<uint8_t>& data,
    std::string& workload_id,
    WorkloadDAG& dag) {

    Envelope env;
    if (!env.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
        return false;
    }
    if (!env.has_workload_submission()) {
        return false;
    }

    const auto& sub = env.workload_submission();
    workload_id = sub.workload_id();

    // Two passes: every task must exist before edges are added.
    for (const auto& spec : sub.tasks()) {
        Task task;
        task.id = spec.task_id();
        task.name = spec.name();
        task.profile.compute_cost = Duration{static_cast<int64_t>(spec.profile().compute_cost_us())};
        task.profile.memory_bytes = spec.profile().memory_bytes();
        task.profile.input_bytes = spec.profile().input_bytes();
        task.profile.output_bytes = spec.profile().output_bytes();
        dag.add_task(std::move(task));
    }
    for (const auto& spec : sub.tasks()) {
        for (const auto& dep : spec.dependencies()) {
            dag.add_dependency(dep, spec.task_id());
        }
    }

    return dag.is_valid() && !dag.has_cycle();
}

// ─────────────────────────────────────────────
// Workload Result
// ─────────────────────────────────────────────

std::vector<uint8_t> OffloadCodec::encode_workload_result(
    const std::string& workload_id,
    bool accepted,
    const std::string& error_msg,
    const OrchestrationResult& result) {

    Envelope env;
    auto* res = env.mutable_workload_result();
    res->set_workload_id(workload_id);
    res->set_accepted(accepted);
    res->set_error_message(error_msg);
    res->set_local_tasks(result.local_tasks);
    res->set_offloaded_tasks(result.offloaded_tasks);
    res->set_completed_tasks(result.completed_tasks);
    res->set_failed_tasks(result.failed_tasks);
    res->set_offload_fallbacks(result.offload_fallbacks);
    res->set_makespan_estimate_us(static_cast<uint64_t>(result.plan.estimated_makespan.count()));
    res->set_total_duration_us(static_cast<uint64_t>(result.total_duration.count()));

    std::vector<uint8_t> buf(env.ByteSizeLong());
    env.SerializeToArray(buf.data(), static_cast<int>(buf.size()));
    return buf;
}

bool OffloadCodec::decode_workload_result(
    const std::vector<uint8_t>& data,
    std::string& workload_id,
    bool& accepted,
    std::string& error_msg,
    OrchestrationResult& result) {

    Envelope env;
    if (!env.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
        return false;
    }
    if (!env.has_workload_result()) {
        return false;
    }

    const auto& res = env.workload_result();
    workload_id = res.workload_id();
    accepted = res.accepted();
    error_msg = res.error_message();
    result.local_tasks = res.local_tasks();
    result.offloaded_tasks = res.offloaded_tasks();
    result.completed_tasks = res.completed_tasks();
    result.failed_tasks = res.failed_tasks();
    result.offload_fallbacks = res.offload_fallbacks();
    result.plan.estimated_makespan = Duration{static_cast<int64_t>(res.makespan_estimate_us())};
    result.total_duration = Duration{static_cast<int64_t>(res.total_duration_us())};

    return true;
}

}  // namespace edge_orchestrator
