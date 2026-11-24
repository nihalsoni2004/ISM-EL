#include "benchmark/rebuild_study.h"

#include "core/engine.h"
#include "files/sha256.h"
#include "recovery/rebuild_manager.h"
#include "recovery/scheduling/json.h"

#include <algorithm>
#include <fstream>
#include <future>
#include <set>
#include <thread>

namespace raid::benchmark {
namespace {
using telemetry::Clock;
using scheduling::TraceEvent;
constexpr std::size_t traceLimit = 8192;
const auto blockBytes = raidBlockSize;

void only(const Json& object, const std::set<std::string_view>& fields) {
    if (!object.is_object()) throw std::invalid_argument("Expected a rebuild study object");
    for (const auto& item : object.items())
        if (!fields.count(item.key())) throw std::invalid_argument("Unknown study field: " + item.key());
}
std::uint64_t ns(Clock::duration diff) noexcept {
    return diff <= Clock::duration::zero() ? 0 :
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(diff).count());
}
void checkTime(Clock::time_point deadline) {
    if (Clock::now() >= deadline) throw std::runtime_error("DEADLINE_EXCEEDED");
}
fs::path image(const fs::path& root, std::string_view id) {
    return root / config::defaultDataDirectory / (std::string(id) + ".img");
}
std::string digest(const fs::path& path) {
    Sha256 hash;
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Could not open image for integrity verification");
    std::array<char, 64 * 1024> bytes{};
    while (file.read(bytes.data(), bytes.size()) || file.gcount() > 0)
        hash.update(bytes.data(), static_cast<std::size_t>(file.gcount()));
    if (!file.eof() || file.bad()) throw std::runtime_error("Image integrity read failed");
    return hash.finish();
}

struct Fixture {
    std::vector<std::string> members;
    std::vector<std::string> hashes;
    std::uint64_t capacity = 0;
};

void verifyContents(RaidController& array, std::uint64_t seed, Clock::time_point deadline) {
    const telemetry::Scope maintenance(telemetry::Origin::Internal, "array0");
    constexpr auto span = 64ULL;
    std::vector<std::uint8_t> payload(span * blockBytes), expected(payload.size());
    for (std::uint64_t lba = 0; lba < array.capacityBlocks(); lba += span) {
        checkTime(deadline);
        const auto blocks = std::min<std::uint64_t>(span, array.capacityBlocks() - lba);
        for (std::uint64_t offset = 0; offset < blocks; ++offset)
            fillBlock(expected.data() + offset * blockBytes, seed, lba + offset, 0);
        array.readBlocks(lba, blocks, payload.data(), payload.size());
        if (!std::equal(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(blocks * blockBytes), expected.begin()))
            throw std::runtime_error("REFERENCE_DATA_MISMATCH");
    }
    if (array.status() == RaidHealth::Healthy) array.verify();
}
Fixture prepare(const Study& study, const fs::path& root, Clock::time_point deadline) {
    Fixture result;
    {
        StorageEngine engine;
        engine.initialize(root);
        for (std::uint64_t i = 0; i < study.members; ++i) {
            const auto id = "disk" + std::to_string(i);
            engine.disks().createDisk(id, study.diskMiB * mib);
            result.members.push_back(id);
        }
        engine.disks().createDisk("disk90", study.diskMiB * mib);
        engine.arrays().registerSpare("disk90");
        auto& array = engine.arrays().createArray("array0", study.level, result.members,
            study.level == RaidLevel::Raid1 ? 1 : 16);
        result.capacity = array.capacityBlocks();
        const auto row = dataUnitCount(study.level, result.members.size()) * array.configuration().stripeUnitBlocks;
        const auto stride = std::max<std::uint64_t>(row, (192 / row) * row);
        std::vector<std::uint8_t> data(stride * blockBytes);
        const telemetry::Scope maintenance(telemetry::Origin::Internal, "array0");
        for (std::uint64_t lba = 0; lba < result.capacity; lba += stride) {
            checkTime(deadline);
            const auto blocks = std::min(stride, result.capacity - lba);
            for (std::uint64_t offset = 0; offset < blocks; ++offset)
                fillBlock(data.data() + offset * blockBytes, study.seed, lba + offset, 0);
            array.writeBlocks(lba, blocks, data.data(), data.size());
        }
        verifyContents(array, study.seed, deadline);
        engine.shutdown();
    }
    for (const auto& id : result.members) result.hashes.push_back(digest(image(root, id)));
    {
        StorageEngine engine;
        engine.initialize(root);
        engine.arrays().failMember("array0", "disk0");
    }
    return result;
}

void traceProgress(const std::vector<TraceEvent>& traces, std::uint64_t total, std::uint64_t batch) {
    std::uint64_t expected = 0;
    for (const auto& event : traces) {
        if (event.kind == TraceEvent::Kind::Reconstruction) {
            if (expected == total) throw std::runtime_error("DUPLICATE_COMPLETION_CHECKPOINT");
            expected += std::min(batch, total - expected);
            if (!expected || event.completedBlocks != expected)
                throw std::runtime_error("NON_MONOTONIC_CHECKPOINT");
        }
    }
    if (expected != total) throw std::runtime_error("INCOMPLETE_CHECKPOINTS");
}

std::vector<ReadSample> schedule(const LoadProfile& profile) {
    std::vector<ReadSample> result;
    std::uint64_t offset = 0;
    for (const auto& phase : profile.phases) {
        const auto window = phase.durationMs * 1000000;
        if (phase.rate) {
            for (std::uint64_t i = 0;; ++i) {
                const auto t = i * 1000000000 / phase.rate;
                if (t >= window) break;
                ReadSample request;
                request.scheduledNs = offset + t;
                result.push_back(request);
            }
        }
        offset += window;
    }
    return result;
}

}  // namespace

Study readStudy(const fs::path& path) {
    if (!fs::is_regular_file(path) || fs::file_size(path) > 65536)
        throw std::invalid_argument("Expected a study JSON file of at most 64 KiB");
    std::ifstream file(path);
    Json input;
    try { file >> input; } catch (const Json::exception& exc) { throw std::invalid_argument(exc.what()); }
    only(input, {"schema_version", "source", "raid_level", "member_capacity_mib", "batch_blocks", "repetitions",
                 "seed", "runtime_seconds", "conditions", "loads", "scheduling", "design_note"});
    using scheduling::uintField;
    if (uintField(input, "schema_version", 0, 1, 1) != 1 || input.value("source", std::string{}) != "MEASURED")
        throw std::invalid_argument("Expected schema-1 MEASURED rebuild study");
    const auto level = uintField(input, "raid_level", 1, 1, 6);
    if (level != 1 && level != 5 && level != 6) throw std::invalid_argument("Use RAID 1, 5 or 6");
    Study result{};
    result.level = static_cast<RaidLevel>(level);
    result.members = level == 1 ? 2 : 4;
    result.diskMiB = uintField(input, "member_capacity_mib", 4, 1, 16);
    result.batchBlocks = uintField(input, "batch_blocks", 16, 8, 64);
    result.repetitions = uintField(input, "repetitions", 5, 1, 10);
    result.seed = uintField(input, "seed", 12345, 0, UINT64_MAX);
    result.runtimeSeconds = uintField(input, "runtime_seconds", 650, 5, 900);
    result.scheduling = scheduling::fromJson(input.value("scheduling", Json::object()));
    if (!input.contains("conditions") || !input["conditions"].is_array() || input["conditions"].empty() ||
        input["conditions"].size() > 4 || !input.contains("loads") || !input["loads"].is_array() ||
        input["loads"].empty() || input["loads"].size() > 5)
        throw std::invalid_argument("Supply bounded policy and workload lists");
    std::set<std::string> seen;
    for (const auto& item : input["conditions"]) {
        const auto label = item.get<std::string>();
        if (label != "DEGRADED_ONLY") (void)scheduling::parsePolicy(label);
        if (!seen.insert(label).second) throw std::invalid_argument("Duplicate policy condition");
        result.conditions.push_back(label);
    }
    seen.clear();
    std::uint64_t maxRequests = 0;
    for (const auto& load : input["loads"]) {
        only(load, {"name", "phases"});
        LoadProfile profile{load.at("name").get<std::string>(), {}};
        if (!safeName(profile.id) || !seen.insert(profile.id).second || !load.at("phases").is_array() ||
            load.at("phases").empty() || load.at("phases").size() > 5)
            throw std::invalid_argument("Invalid foreground profile");
        std::uint64_t duration = 0, requests = 0;
        for (const auto& phase : load["phases"]) {
            only(phase, {"duration_ms", "offered_iops"});
            const auto ms = uintField(phase, "duration_ms", 0, 50, 5000);
            if (!ms) throw std::invalid_argument("Missing foreground phase duration");
            const auto rate = uintField(phase, "offered_iops", 0, 0, 500);
            duration += ms;
            requests += (ms * rate + 999) / 1000;
            profile.phases.push_back({ms, rate});
        }
        if (duration > 5000 || requests > 2500) throw std::invalid_argument("Foreground load budget exceeded");
        maxRequests += requests;
        result.profiles.push_back(std::move(profile));
    }
    if (maxRequests * result.repetitions * result.conditions.size() > 100000)
        throw std::invalid_argument("Study exceeds 100,000 scheduled foreground requests");
    result.normalized = input;
    result.normalized["scheduling"] = scheduling::toJson(result.scheduling);
    result.normalized["member_capacity_mib"] = result.diskMiB;
    result.normalized["batch_blocks"] = result.batchBlocks;
    result.normalized["raid_level"] = level;
    result.normalized["repetitions"] = result.repetitions;
    result.normalized["seed"] = result.seed;
    result.normalized["runtime_seconds"] = result.runtimeSeconds;
    return result;
}

StudyTrial runTrial(const Study& study, std::string_view condition, const LoadProfile& profile,
                    std::uint64_t repetition, const fs::path& root, Clock::time_point deadline) {
    StudyTrial trial;
    for (const auto& phase : profile.phases) trial.windowNs += phase.durationMs * 1000000;
    trial.reads = schedule(profile);
    const bool control = condition == "DEGRADED_ONLY";
    std::atomic_bool active{false}, stop{false};
    std::promise<Clock::time_point> ready;
    auto checkpoint = ready.get_future();
    std::future<RebuildJob> worker;
    Clock::time_point origin{};
    bool overflow = false;
    try {
        checkTime(deadline);
        const auto fixture = prepare(study, root, deadline);
        Config generator{};
        generator.workload = Workload::ReadBaseline;
        generator.blocks = 1;
        generator.operations = trial.reads.size();
        generator.footprint = fixture.capacity;
        generator.seed = study.seed;
        const auto operations = makeWorkload(generator, repetition);
        for (std::size_t i = 0; i < trial.reads.size(); ++i) trial.reads[i].lba = operations[i].lba;

        auto policy = study.scheduling;
        policy.policy = control ? scheduling::Policy::FullSpeed : scheduling::parsePolicy(condition);
        RebuildManager manager(root, study.batchBlocks, {}, [&](const TraceEvent& event) {
            if (trial.traces.size() < traceLimit) trial.traces.push_back(event);
            else overflow = true;
            if (event.kind == TraceEvent::Kind::Started) {
                active.store(true);
                ready.set_value(event.at - std::chrono::nanoseconds(event.elapsedNs));
            }
            if (event.kind == TraceEvent::Kind::Completed || event.kind == TraceEvent::Kind::Paused)
                active.store(false);
        });
        const auto job = manager.prepare("array0", "disk0", "disk90", policy);
        trial.jobId = job.id;
        trial.jobState = std::string(rebuildStatusName(job.status));
        trial.traces.reserve(traceLimit);
        scheduling::resetObservations();
        origin = Clock::now();
        if (!control) {
            worker = std::async(std::launch::async, [&] {
                try {
                    auto done = manager.run(job.id, 0, [&] { return stop.load() || Clock::now() >= deadline; });
                    active.store(false);
                    return done;
                } catch (...) {
                    active.store(false);
                    throw;
                }
            });
        }

        std::array<std::uint8_t, blockBytes> data{}, expected{};
        bool allCorrect = true;
        try {
            if (!control) {
                while (checkpoint.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready) {
                    checkTime(deadline);
                    if (worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                        throw std::runtime_error("REBUILD_FAILED_TO_START");
                }
                origin = checkpoint.get();
            }
            for (auto& request : trial.reads) {
                const auto scheduled = origin + std::chrono::nanoseconds(request.scheduledNs);
                if (scheduled > deadline || Clock::now() >= deadline) {
                    request.error = "DEADLINE_DROP";
                    allCorrect = false;
                    continue;
                }
                std::this_thread::sleep_until(scheduled);
                request.submitNs = ns(Clock::now() - origin);
                request.active = active.load();
                {
                    scheduling::ReadDemand demand;
                    try {
                        {
                            StorageEngine engine;
                            engine.initialize(root);
                            auto& array = engine.arrays().openArray("array0");
                            request.active = request.active && array.status() == RaidHealth::Degraded;
                            request.operationStartNs = ns(Clock::now() - origin);
                            array.readBlocks(request.lba, 1, data.data(), data.size());
                            request.operationEndNs = ns(Clock::now() - origin);
                        }
                        request.success = true;
                        demand.success(1);
                    } catch (const std::exception&) {
                        request.error = "FOREGROUND_IO_FAILED";
                    }
                    request.completionNs = ns(Clock::now() - origin);
                }
                if (request.success) {
                    fillBlock(expected.data(), study.seed, request.lba, 0);
                    request.validated = expected == data;
                    if (!*request.validated) request.error = "FOREGROUND_DATA_MISMATCH";
                }
                allCorrect = allCorrect && request.success && request.validated == true;
            }
            const auto windowEnd = origin + std::chrono::nanoseconds(trial.windowNs);
            if (windowEnd <= deadline) std::this_thread::sleep_until(windowEnd);
            else allCorrect = false;
        } catch (...) {
            stop.store(true);
            if (worker.valid()) worker.wait();
            throw;
        }

        if (worker.valid()) {
            const auto completed = worker.get();
            trial.jobState = std::string(rebuildStatusName(completed.status));
            if (completed.status != RebuildStatus::Completed || completed.completedBlocks != completed.totalBlocks)
                throw std::runtime_error("REBUILD_DID_NOT_COMPLETE");
            traceProgress(trial.traces, completed.totalBlocks, study.batchBlocks);
            if (trial.traces.back().kind != TraceEvent::Kind::Completed) throw std::runtime_error("MISSING_COMPLETION_TRACE");
            trial.rebuildNs = trial.traces.back().elapsedNs;
            if (digest(image(root, "disk90")) != fixture.hashes[0])
                throw std::runtime_error("REBUILT_IMAGE_MISMATCH");
            trial.rebuildValidated = true;
        }
        {
            StorageEngine engine;
            engine.initialize(root);
            auto& array = engine.arrays().openArray("array0");
            const auto health = array.status();
            trial.health = raidHealthName(health);
            if (health != (control ? RaidHealth::Degraded : RaidHealth::Healthy))
                throw std::runtime_error("UNEXPECTED_FINAL_HEALTH");
            verifyContents(array, study.seed, deadline);
        }
        for (std::size_t i = 0; i < fixture.members.size(); ++i)
            if (digest(image(root, fixture.members[i])) != fixture.hashes[i])
                throw std::runtime_error("SOURCE_IMAGE_CHANGED");
        trial.sourceUnchanged = true;
        if (overflow) throw std::runtime_error("TRACE_BUDGET_EXCEEDED");
        if (!allCorrect) throw std::runtime_error("FOREGROUND_VALIDATION_FAILED");
        trial.foregroundValidated = true;
        trial.passed = true;
    } catch (const std::exception& exception) {
        stop.store(true);
        if (worker.valid()) worker.wait();
        trial.error = exception.what();
        for (auto& request : trial.reads)
            if (!request.submitNs && request.error.empty()) request.error = "ABORTED";
    }
    return trial;
}
}  // namespace raid::benchmark
