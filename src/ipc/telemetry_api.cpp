#include "ipc/telemetry_api.h"

#include "telemetry/telemetry.h"

namespace raid::ipc {
namespace {

using namespace telemetry;

Json observation(Window window) {
    return {{"generation", window.generation}, {"start_offset_ns", window.startOffsetNs},
            {"end_offset_ns", window.endOffsetNs},
            {"seconds", static_cast<double>(window.endOffsetNs - window.startOffsetNs) / 1000000000.0}};
}

Json counts(const Counters& value) {
    return {{"attempted", value.attempted}, {"completed", value.completed}, {"failed", value.failed},
            {"requested_blocks", value.requestedBlocks}, {"requested_bytes", value.requestedBytes},
            {"blocks", value.blocks}, {"bytes", value.bytes}, {"elapsed_ns", value.elapsedNs}};
}

Json seriesJson(Kind kind, const Series& series, bool includeSamples) {
    auto json = counts(series.total);
    json["category"] = name(kind);
    auto origins = Json::object();
    for (std::size_t index = 0; index < originCount; ++index) {
        origins[std::string(name(static_cast<Origin>(index)))] = counts(series.byOrigin[index]);
    }
    json["origin_counts"] = std::move(origins);
    auto latency = Json{{"count", series.total.attempted}, {"sample_count", series.samples.size()},
        {"window", series.samples.empty() ? "EMPTY" :
            series.total.attempted == series.samples.size() ? "ENTIRE_OBSERVATION" : "RECENT_SAMPLES"},
        {"min_ms", nullptr}, {"max_ms", nullptr}, {"mean_ms", nullptr},
        {"p50_ms", nullptr}, {"p95_ms", nullptr}, {"p99_ms", nullptr}};
    if (const auto summary = summarize(series.samples)) {
        latency.update(Json{{"min_ms", summary->minMs}, {"max_ms", summary->maxMs}, {"mean_ms", summary->meanMs},
            {"p50_ms", summary->p50Ms}, {"p95_ms", summary->p95Ms}, {"p99_ms", summary->p99Ms}});
    }
    json["latency"] = std::move(latency);
    if (includeSamples) json["latency_samples_ns"] = series.samples;
    return json;
}

Json optionalId(const Identifier& id) {
    return id.view().empty() ? Json(nullptr) : Json(id.view());
}

Json eventJson(const Event& event) {
    return {{"category", name(event.kind)}, {"origin", name(event.context.origin)}, {"array_id", optionalId(event.context.array)},
            {"disk_id", optionalId(event.disk)}, {"job_id", optionalId(event.context.job)},
            {"requested_blocks", event.requestedBlocks}, {"requested_bytes", event.requestedBytes},
            {"blocks", event.passed ? event.requestedBlocks : 0}, {"bytes", event.passed ? event.requestedBytes : 0},
            {"elapsed_ns", event.elapsedNs}, {"end_offset_ns", event.endOffsetNs},
            {"success", event.passed}, {"error_category", event.passed ? Json(nullptr) : Json("OPERATION_FAILED")}};
}

Json parameters(const performance::Parameters& settings) {
    return {{"sequential_read_ms", settings.sequentialReadMs}, {"sequential_write_ms", settings.sequentialWriteMs},
            {"random_read_ms", settings.randomReadMs}, {"random_write_ms", settings.randomWriteMs},
            {"read_mib_per_second", settings.readMiBps}, {"write_mib_per_second", settings.writeMiBps},
            {"queue_depth", settings.queueDepth}};
}

}  // namespace

Json telemetrySnapshot(bool includeSamples, std::size_t recentEvents) {
    const auto snapshot = collector().snapshot(recentEvents);
    auto categories = Json::array();
    for (std::size_t index = 0; index < kindCount; ++index) {
        categories.push_back(seriesJson(static_cast<Kind>(index), snapshot.series[index], includeSamples));
    }
    auto events = Json::array();
    for (const auto& event : snapshot.events) events.push_back(eventJson(event));

    const auto& logicalRead = snapshot.series[static_cast<std::size_t>(Kind::RaidRead)].byOrigin[static_cast<std::size_t>(Origin::Foreground)];
    const auto& logicalWrite = snapshot.series[static_cast<std::size_t>(Kind::RaidWrite)].byOrigin[static_cast<std::size_t>(Origin::Foreground)];
    const auto elapsed = observation(snapshot.window);
    const auto seconds = elapsed.at("seconds").get<double>();
    const auto completed = logicalRead.completed + logicalWrite.completed;
    const auto bytes = logicalRead.bytes + logicalWrite.bytes;
    const bool available = seconds >= 1.0 && (logicalRead.attempted != 0 || logicalWrite.attempted != 0);
    return {{"source", "MEASURED"}, {"scope", "engine_process"}, {"observation", elapsed},
            {"latency_sample_limit_per_category", sampleLimit}, {"categories", std::move(categories)},
            {"recent_events", std::move(events)}, {"logical_rates", {
                {"origin", "FOREGROUND"}, {"completed_requests", completed}, {"bytes", bytes},
                {"iops", available ? Json(static_cast<double>(completed) / seconds) : Json(nullptr)},
                {"mib_per_second", available ? Json((static_cast<double>(bytes) / (1024.0 * 1024.0)) / seconds) : Json(nullptr)}}}};
}

Json resetTelemetry() {
    return {{"source", "MEASURED"}, {"scope", "engine_process"}, {"observation", observation(collector().reset())}};
}

Json rebuildTelemetry(const RebuildJob& job, Json durable) {
    const auto data = collector().job(job.id);
    auto categories = Json::array();
    for (std::size_t index = 0; index < data.series.size(); ++index) {
        categories.push_back(seriesJson(static_cast<Kind>(static_cast<std::size_t>(Kind::RebuildRead) + index),
                                        data.series[index], true));
    }
    return {{"source", "MEASURED"}, {"scope", "engine_process"}, {"observation", observation(data.window)},
            {"job", std::move(durable)}, {"raid_level", static_cast<int>(job.level)},
            {"remaining_blocks", job.totalBlocks - job.completedBlocks},
            {"last_checkpoint_at", job.checkpoint ? Json(job.checkpoint) : Json(nullptr)},
            {"retained", data.retained}, {"latency_sample_limit_per_category", jobSampleLimit}, {"categories", std::move(categories)}};
}

Json modelProfiles() {
    auto all = Json::array();
    for (const auto& profile : performance::profiles()) {
        all.push_back({{"profile", profile.id}, {"parameters", parameters(profile.settings)}});
    }
    return {{"source", "MODELED"}, {"profiles", std::move(all)},
            {"limits", {{"max_requests", performance::maxRequests}, {"max_devices", performance::maxDevices},
                        {"max_bytes_per_request", performance::maxBytes}, {"max_dependencies", performance::maxDependencies},
                        {"max_arrival_ms", performance::maxArrivalMs}, {"max_queue_depth", performance::maxQueueDepth}}}};
}

Json modelSimulation(const performance::Profile& profile, const performance::Parameters& settings,
                     std::size_t devices, const std::vector<performance::Request>& requests) {
    const auto result = performance::simulate(settings, devices, requests);
    auto rows = Json::array();
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto& input = requests[index];
        const auto& output = result.requests[index];
        rows.push_back({{"index", index}, {"device", input.device}, {"bytes", input.bytes},
                        {"operation", input.operation == performance::Operation::Read ? "READ" : "WRITE"},
                        {"access", input.access == performance::Access::Random ? "RANDOM" : "SEQUENTIAL"},
                        {"depends_on", input.dependencies}, {"requested_arrival_ms", input.arrivalMs},
                        {"effective_arrival_ms", output.arrivalMs}, {"start_ms", output.startMs},
                        {"service_ms", output.serviceMs}, {"queue_wait_ms", output.queueWaitMs},
                        {"completion_ms", output.completionMs}});
    }
    return {{"source", "MODELED"}, {"profile", profile.id}, {"parameters", parameters(settings)},
            {"queue_policy", "FIFO_EFFECTIVE_ARRIVAL"}, {"devices", devices}, {"request_count", requests.size()},
            {"first_arrival_ms", result.firstArrivalMs}, {"last_completion_ms", result.lastCompletionMs},
            {"total_modeled_elapsed_ms", result.elapsedMs()}, {"requests", std::move(rows)}};
}

}  // namespace raid::ipc
