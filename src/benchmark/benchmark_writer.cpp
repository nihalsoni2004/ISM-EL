#include "benchmark/benchmark.h"
#include "benchmark/rebuild_study.h"

#include "core/config.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace raid::benchmark {
namespace {

using telemetry::Clock;
constexpr std::string_view rawFile = "benchmark_runs.csv";
constexpr std::string_view summaryFile = "benchmark_results.csv";
constexpr std::string_view manifestFile = "manifest.json";
constexpr std::string_view cachePolicy = "HOST_CACHE_ENABLED_PREPARED";

void guardPath(const fs::path& path) {
    if (!path.is_absolute() || path.lexically_normal() != path) throw std::invalid_argument("Expected a normalized absolute path");
    auto current = path.root_path();
    for (const auto& component : path.relative_path()) {
        current /= component;
        const auto attr = GetFileAttributesW(current.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) continue;
            throw std::system_error(static_cast<int>(error), std::system_category(), "Check benchmark path");
        }
        if ((attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0) throw std::invalid_argument("Benchmark paths cannot traverse reparse points");
    }
}

class Handle {
public:
    explicit Handle(HANDLE handle) : handle_(handle) {
        if (handle_ == INVALID_HANDLE_VALUE)
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Open exclusive benchmark file");
    }
    ~Handle() { CloseHandle(handle_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const noexcept { return handle_; }
private:
    HANDLE handle_;
};

class ResultFile {
public:
    explicit ResultFile(const fs::path& path, std::uint64_t limit = 16 * mib)
        : file_(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr)), limit_(limit) {}
    void write(std::string_view content) {
        if (content.size() > limit_ - written_) throw std::runtime_error("Result file exceeds its size budget");
        DWORD count = 0;
        if (!WriteFile(file_.get(), content.data(), static_cast<DWORD>(content.size()), &count, nullptr) ||
            count != content.size()) throw std::runtime_error("Write benchmark result file failed");
        written_ += count;
    }
    void flush() {
        if (!FlushFileBuffers(file_.get())) throw std::runtime_error("Flush benchmark result file failed");
    }
private:
    Handle file_;
    std::uint64_t limit_;
    std::uint64_t written_ = 0;
};

std::string utc(const char* format) {
    const auto now = std::time(nullptr);
    std::tm value{};
    if (gmtime_s(&value, &now) != 0) throw std::runtime_error("Read time for benchmark manifest failed");
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::put_time(&value, format);
    return out.str();
}

std::string newRunId() {
    std::random_device random;
    std::ostringstream out;
    out << "run-" << utc("%Y%m%dT%H%M%SZ") << '-' << std::hex << random() << random();
    return out.str();
}

Json opt(const std::optional<std::uint64_t>& value) { return value ? Json(*value) : Json(nullptr); }

std::string row(const Json& values) {
    std::string line;
    bool first = true;
    for (const auto& field : values) {
        if (!first) line += ',';
        first = false;
        if (field.is_string()) line += escapeCsv(field.get_ref<const std::string&>());
        else if (!field.is_null()) line += field.dump();
    }
    line += '\n';
    return line;
}

std::string csvHeader(const Json& columns) { return row(columns); }

void removeWorkspace(const fs::path& expectedParent, const fs::path& target) {
    guardPath(target);
    if (target.parent_path() != expectedParent || !fs::is_directory(target) ||
        fs::canonical(target).parent_path() != fs::canonical(expectedParent))
        throw std::runtime_error("Cannot verify workspace ownership for cleanup");
    for (const auto& entry : fs::recursive_directory_iterator(target)) {
        guardPath(entry.path());
        if (!entry.is_regular_file() && !entry.is_directory())
            throw std::runtime_error("Unexpected workspace entry during cleanup");
    }
    fs::remove_all(target);
}

void requireDirectory(const fs::path& path) {
    guardPath(path);
    fs::create_directories(path);
    guardPath(path);
    if (!fs::is_directory(path)) throw std::invalid_argument("Expected benchmark directory");
}

std::unique_ptr<Handle> lockWorkspace(const fs::path& workspaces) {
    const auto lockPath = workspaces / ".runner.lock";
    guardPath(lockPath);
    auto lock = std::make_unique<Handle>(CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION metadata{};
    if (!GetFileInformationByHandle(lock->get(), &metadata) ||
        (metadata.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        throw std::invalid_argument("Benchmark process lock must be a regular file");
    return lock;
}

const Json& columns() {
    static const Json headers = {
        "schema_version", "run_id", "repetition", "benchmark_name", "raid_level", "disk_count", "disk_capacity_mib",
        "raid_raw_capacity_bytes", "raid_usable_capacity_bytes", "stripe_unit_blocks", "capacity_mode", "footprint_blocks",
        "workload", "request_size_bytes", "operation_count", "warmup_operations", "seed", "queue_depth", "mixed_read_percent",
        "thin_offset_blocks", "validation", "validation_stride", "measurement_source", "cache_policy", "build_type",
        "compiler", "rebuild_contention", "elapsed_seconds", "preparation_seconds", "warmup_seconds", "attempted_operations",
        "completed_operations", "failed_operations", "logical_bytes", "logical_written_blocks", "iops", "throughput_mib_s",
        "latency_count", "latency_min_ms", "latency_max_ms", "latency_mean_ms", "latency_p50_ms", "latency_p95_ms",
        "latency_p99_ms", "physical_read_requests", "physical_write_requests", "physical_read_blocks", "physical_write_blocks",
        "physical_bytes_read", "physical_bytes_written", "write_amplification_blocks", "new_chunks", "allocated_before_bytes",
        "allocated_after_bytes", "rebuild_seconds", "rebuilt_blocks", "rebuild_total_blocks", "rebuild_overlap_requests",
        "rebuild_state", "validation_status", "status", "error"
    };
    return headers;
}

Json hostLabel(const wchar_t* label) {
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, label, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1 || count > 1024) return nullptr;
    std::string text(count, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, label, -1, text.data(), count, nullptr, nullptr))
        return nullptr;
    text.pop_back();
    return text;
}

Json host() {
    std::array<wchar_t, 256> processor{}, model{};
    const auto size = GetEnvironmentVariableW(L"PROCESSOR_IDENTIFIER", processor.data(), static_cast<DWORD>(processor.size()));
    DWORD modelBytes = static_cast<DWORD>(sizeof(model));
    const auto found = RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                   L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, model.data(), &modelBytes);
    return {{"os", BENCHMARK_OS}, {"os_version", BENCHMARK_OS_VERSION},
            {"cpu_identifier", size != 0 && size < processor.size() ? hostLabel(processor.data()) : Json(nullptr)},
            {"cpu_model", found == ERROR_SUCCESS ? hostLabel(model.data()) : Json(nullptr)}, {"host_storage", nullptr}};
}

void writeSamples(ResultFile& raw, const Config& c, const std::string& id,
                  std::uint64_t rep, const Repetition& result) {
    for (std::size_t index = 0; index < result.samples.size(); ++index) {
        const auto& sample = result.samples[index];
        const auto& request = sample.operation;
        const auto bytes = request.blocks * raidBlockSize;
        raw.write(row({1, id, rep, index, c.name, static_cast<int>(c.level), name(c.workload),
            request.write ? "WRITE" : "READ", bytes, request.lba, request.blocks, request.pattern,
            sample.durationNs, sample.success, sample.validation ? Json(*sample.validation) : Json(nullptr),
            sample.success ? bytes : 0, sample.physical.read.attempted, sample.physical.write.attempted,
            sample.physical.read.blocks, sample.physical.write.blocks, sample.physical.read.bytes,
            sample.physical.write.bytes, sample.newChunks, sample.rebuildOverlap, sample.error, "MEASURED"}));
    }
}

Json writeSummary(ResultFile& summary, const Config& c, const std::string& id,
                  std::uint64_t rep, const Repetition& result) {
    auto values = summarizeRun(c, result);
    values.update(Json{{"schema_version", 1}, {"run_id", id}, {"repetition", rep}, {"benchmark_name", c.name},
        {"raid_level", static_cast<int>(c.level)}, {"disk_count", c.disks}, {"disk_capacity_mib", c.diskMiB},
        {"raid_raw_capacity_bytes", c.diskMiB * c.disks * mib}, {"raid_usable_capacity_bytes", usableBlocks(c) * raidBlockSize},
        {"stripe_unit_blocks", c.stripe}, {"capacity_mode", name(c.capacityMode)}, {"footprint_blocks", c.footprint},
        {"workload", name(c.workload)}, {"request_size_bytes", c.blocks * raidBlockSize}, {"operation_count", c.operations},
        {"warmup_operations", c.warmup}, {"seed", c.seed}, {"queue_depth", 1}, {"mixed_read_percent", c.readPercent},
        {"thin_offset_blocks", c.thinOffset}, {"validation", name(c.validation)}, {"validation_stride", c.sampleStride},
        {"measurement_source", "MEASURED"}, {"cache_policy", cachePolicy}, {"build_type", BENCHMARK_BUILD_TYPE},
        {"compiler", BENCHMARK_COMPILER}, {"rebuild_contention", c.rebuild}});
    auto ordered = Json::array();
    for (const auto& col : columns()) ordered.push_back(values.at(col.get_ref<const std::string&>()));
    summary.write(row(ordered));
    return {{"repetition", rep}, {"status", values.at("status")}, {"error", result.error},
            {"validation_status", values.at("validation_status")}};
}

}  // namespace

std::string escapeCsv(std::string_view value) {
    if (value.find_first_of(",\"\r\n") == std::string_view::npos) return std::string(value);
    std::string result = "\"";
    for (char character : value) {
        result += character;
        if (character == '"') result += '"';
    }
    return result + '"';
}

Json summarizeRun(const Config& c, const Repetition& run) {
    std::uint64_t completed = 0, logicalBytes = 0, logicalWrites = 0, readRequests = 0, writeRequests = 0;
    std::uint64_t readBlocks = 0, writeBlocks = 0, readBytes = 0, writeBytes = 0, chunks = 0, overlaps = 0;
    std::vector<std::uint64_t> latencies;
    latencies.reserve(run.samples.size());
    for (const auto& sample : run.samples) {
        if (sample.success) {
            ++completed;
            logicalBytes += sample.operation.blocks * raidBlockSize;
            if (sample.operation.write) logicalWrites += sample.operation.blocks;
        }
        readRequests += sample.physical.read.attempted;
        writeRequests += sample.physical.write.attempted;
        readBlocks += sample.physical.read.blocks;
        writeBlocks += sample.physical.write.blocks;
        readBytes += sample.physical.read.bytes;
        writeBytes += sample.physical.write.bytes;
        chunks += sample.newChunks;
        overlaps += sample.rebuildOverlap;
        latencies.push_back(sample.durationNs);
    }
    const auto count = latencies.size();
    const auto latency = telemetry::summarize(std::move(latencies));
    const auto passed = valid(c, run);
    return {{"elapsed_seconds", run.elapsed}, {"preparation_seconds", run.preparation}, {"warmup_seconds", run.warmup},
        {"attempted_operations", run.samples.size()}, {"completed_operations", completed},
        {"failed_operations", run.samples.size() - completed}, {"logical_bytes", logicalBytes}, {"logical_written_blocks", logicalWrites},
        {"iops", run.elapsed > 0 ? Json(static_cast<double>(completed) / run.elapsed) : Json(nullptr)},
        {"throughput_mib_s", run.elapsed > 0 ? Json(static_cast<double>(logicalBytes) / mib / run.elapsed) : Json(nullptr)},
        {"latency_count", count}, {"latency_min_ms", latency ? Json(latency->minMs) : Json(nullptr)},
        {"latency_max_ms", latency ? Json(latency->maxMs) : Json(nullptr)}, {"latency_mean_ms", latency ? Json(latency->meanMs) : Json(nullptr)},
        {"latency_p50_ms", latency ? Json(latency->p50Ms) : Json(nullptr)}, {"latency_p95_ms", latency ? Json(latency->p95Ms) : Json(nullptr)},
        {"latency_p99_ms", latency ? Json(latency->p99Ms) : Json(nullptr)}, {"physical_read_requests", readRequests},
        {"physical_write_requests", writeRequests}, {"physical_read_blocks", readBlocks}, {"physical_write_blocks", writeBlocks},
        {"physical_bytes_read", readBytes}, {"physical_bytes_written", writeBytes},
        {"write_amplification_blocks", logicalWrites ? Json(static_cast<double>(writeBlocks) / logicalWrites) : Json(nullptr)},
        {"new_chunks", chunks}, {"allocated_before_bytes", opt(run.allocatedBefore)}, {"allocated_after_bytes", opt(run.allocatedAfter)},
        {"rebuild_seconds", run.rebuildSeconds ? Json(*run.rebuildSeconds) : Json(nullptr)},
        {"rebuilt_blocks", run.rebuilt}, {"rebuild_total_blocks", run.rebuildTotal}, {"rebuild_overlap_requests", overlaps},
        {"rebuild_state", run.rebuildState}, {"validation_status", c.validation == Validation::None ? "NOT_CHECKED" :
            passed && run.persisted ? "PASSED" : run.error == "DATA_MISMATCH" || run.error == "PERSISTED_DATA_MISMATCH" ||
            run.error == "VALIDATION_READ_FAILED" || run.error == "ALLOCATION_MISMATCH" ? "FAILED" : "INCOMPLETE"},
        {"status", passed ? "COMPLETED" : "FAILED"}, {"error", run.error}};
}

bool safeName(std::string_view value) noexcept {
    if (value.empty() || value.size() > 64) return false;
    return std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    });
}
fs::path outputRoot(const fs::path& root, std::string_view group) {
    if (!group.empty() && !safeName(group))
        throw std::invalid_argument("Output directory must be one safe name under benchmarks/results");
    const auto result = root / "results";
    const auto path = group.empty() ? result : result / std::string(group);
    guardPath(path);
    return path;
}

RunResult run(const Config& config, const fs::path& root, std::string_view group, const Json& invocation) {
    const auto outputs = outputRoot(root, group);
    const auto workspaces = root / "workspaces";
    requireDirectory(workspaces);
    requireDirectory(outputs);
    auto lock = lockWorkspace(workspaces);
    const auto estimate = (imageBudgetMiB(config) + 16) * mib + 1024 + config.operations * config.repetitions * 512;
    if (fs::space(workspaces).available < estimate) throw std::runtime_error("Insufficient free space for isolated benchmark");

    const auto id = newRunId();
    const auto results = outputs / id;
    const auto sandbox = workspaces / id;
    if (!fs::create_directory(results)) throw std::runtime_error("Benchmark result ID already exists");
    if (!fs::create_directory(sandbox)) throw std::runtime_error("Benchmark workspace ID already exists");
    guardPath(results);
    guardPath(sandbox);
    ResultFile raw(results / rawFile, config.maxRawBytes);
    ResultFile summary(results / summaryFile);
    raw.write("schema_version,run_id,repetition,sequence,benchmark_name,raid_level,workload,operation,request_size_bytes,"
        "logical_lba,logical_blocks,pattern_ref,duration_ns,success,validation_passed,logical_bytes_completed,"
        "physical_read_requests,physical_write_requests,physical_read_blocks,physical_write_blocks,physical_bytes_read,"
        "physical_bytes_written,new_chunks,rebuild_overlap,error,source\n");
    summary.write(csvHeader(columns()));

    const auto started = utc("%Y-%m-%dT%H:%M:%SZ");
    const auto deadline = Clock::now() + std::chrono::seconds(config.timeoutSeconds);
    auto reports = Json::array();
    bool passed = true;
    for (std::uint64_t rep = 1; rep <= config.repetitions; ++rep) {
        const auto workspace = sandbox / ("rep-" + std::to_string(rep));
        if (!fs::create_directory(workspace)) throw std::runtime_error("Benchmark repetition workspace already exists");
        auto result = measure(config, rep, workspace, deadline);
        try {
            removeWorkspace(sandbox, workspace);
        } catch (const std::exception& error) {
            if (result.error.empty()) result.error = error.what();
        }
        writeSamples(raw, config, id, rep, result);
        reports.push_back(writeSummary(summary, config, id, rep, result));
        if (!valid(config, result)) {
            passed = false;
            break;
        }
    }
    if (fs::is_empty(sandbox)) fs::remove(sandbox);
    raw.flush();
    summary.flush();
    auto manifest = Json{{"schema_version", 1}, {"run_id", id}, {"status", passed ? "COMPLETED" : "FAILED"},
        {"measurement_source", "MEASURED"}, {"started_at_utc", started}, {"finished_at_utc", utc("%Y-%m-%dT%H:%M:%SZ")},
        {"configuration", describe(config)}, {"build_type", BENCHMARK_BUILD_TYPE}, {"compiler", BENCHMARK_COMPILER},
        {"cmake_version", BENCHMARK_CMAKE}, {"git_commit", std::string_view(BENCHMARK_REVISION).empty() ? Json(nullptr) : Json(BENCHMARK_REVISION)},
        {"application_version", raid::config::applicationVersion}, {"host", host()}, {"analysis_python", invocation.value("python", Json(nullptr))},
        {"invocation", invocation}, {"telemetry_methodology_version", 1}, {"cache_policy", cachePolicy},
        {"warmup_policy", "PER_REPETITION_BEFORE_MEASUREMENT"}, {"repetition_policy", "FRESH_STORAGE_PER_REPETITION"},
        {"max_temporary_image_bytes", imageBudgetMiB(config) * mib}, {"raw_capacity_bytes", config.disks * config.diskMiB * mib},
        {"usable_capacity_bytes", usableBlocks(config) * raidBlockSize}, {"raw_csv", rawFile},
        {"summary_csv", summaryFile}, {"repetitions", std::move(reports)}};
    ResultFile metadata(results / manifestFile);
    metadata.write(manifest.dump(2) + "\n");
    metadata.flush();
    return {results, passed};
}

Json info(const fs::path& root, std::string_view group, std::string_view runId) {
    if (!safeName(runId) || runId.substr(0, 4) != "run-") throw std::invalid_argument("Invalid benchmark run ID");
    const auto path = outputRoot(root, group) / std::string(runId) / manifestFile;
    guardPath(path);
    if (!fs::is_regular_file(path) || fs::file_size(path) > mib) throw std::invalid_argument("Benchmark manifest is unavailable");
    std::ifstream stream(path, std::ios::binary);
    return Json::parse(stream);
}

}  // namespace raid::benchmark

namespace raid::benchmark {
namespace {

using TraceKind = scheduling::TraceEvent::Kind;
using telemetry::Clock;

std::string_view stage(TraceKind kind) {
    switch (kind) {
    case TraceKind::Started: return "STARTED";
    case TraceKind::Decision: return "DECISION";
    case TraceKind::Reconstruction: return "RECONSTRUCTION";
    case TraceKind::Verification: return "VERIFICATION";
    case TraceKind::Paused: return "PAUSED";
    case TraceKind::Completed: return "COMPLETED";
    }
    return "UNKNOWN";
}

Json fields(Json prefix, std::initializer_list<Json> tail) {
    for (const auto& value : tail) prefix.push_back(value);
    return prefix;
}
Json millis(const std::optional<std::uint64_t>& value) {
    return value ? Json(telemetry::milliseconds(*value)) : Json(nullptr);
}

void writeTrial(ResultFile& foreground, ResultFile& progress, ResultFile& decisions, ResultFile& summaries,
                Json prefix, const StudyTrial& trial, const LoadProfile& profile, const Study& study) {
    std::uint64_t submitted = 0, completed = 0, failed = 0, dropped = 0, rejected = 0, within = 0, activeCompleted = 0;
    std::vector<std::uint64_t> response, active, queue, operation;
    for (std::size_t index = 0; index < trial.reads.size(); ++index) {
        const auto& sample = trial.reads[index];
        std::optional<std::uint64_t> waited, storage, latency;
        if (sample.submitNs) {
            ++submitted;
            waited = *sample.submitNs >= sample.scheduledNs ? *sample.submitNs - sample.scheduledNs : 0;
            queue.push_back(*waited);
        } else ++dropped;
        if (sample.operationStartNs && sample.operationEndNs) {
            storage = *sample.operationEndNs - *sample.operationStartNs;
            operation.push_back(*storage);
        }
        if (sample.completionNs) {
            latency = *sample.completionNs - sample.scheduledNs;
            if (sample.success) {
                ++completed;
                response.push_back(*latency);
                if (*sample.completionNs < trial.windowNs) ++within;
                if (sample.active) active.push_back(*latency);
                if (sample.active && trial.rebuildNs && *sample.completionNs < *trial.rebuildNs &&
                    *sample.completionNs < trial.windowNs) ++activeCompleted;
            } else ++failed;
        }
        if (sample.validated == false) ++rejected;
        foreground.write(row(fields(prefix, {index, sample.scheduledNs, opt(sample.submitNs), opt(sample.operationStartNs),
            opt(sample.operationEndNs), opt(sample.completionNs), opt(waited), opt(storage), opt(latency),
            sample.lba, 1, sample.success, sample.validated ? Json(*sample.validated) : Json(nullptr),
            sample.active, sample.error, "MEASURED"})));
    }

    std::uint64_t processing = 0, locking = 0, wait = 0, checkpoint = 0, overhead = 0, blocks = 0, totalBlocks = 0;
    std::uint64_t readBlocks = 0, writeBlocks = 0, readRequests = 0, writeRequests = 0;
    std::uint64_t updates = 0, transitions = 0, sumDelay = 0, minDelay = UINT64_MAX, maxDelay = 0;
    std::map<scheduling::Pressure, std::uint64_t> residency;
    const scheduling::TraceEvent* previous = nullptr;
    std::uint64_t validReadSignal = 0;
    for (const auto& event : trial.traces) {
        if (event.kind == TraceKind::Decision) {
            if (previous) {
                residency[previous->decision.pressure] += event.elapsedNs - previous->elapsedNs;
                transitions += previous->decision.pressure != event.decision.pressure;
            }
            previous = &event;
            ++updates;
            validReadSignal += event.load.p95Ms.has_value();
            overhead += event.controllerNs;
            sumDelay += event.decision.delayUs;
            minDelay = std::min(minDelay, event.decision.delayUs);
            maxDelay = std::max(maxDelay, event.decision.delayUs);
            const bool observed = prefix[4] == "ADAPTIVE";
            const auto measured = [&](auto value) { return observed ? Json(value) : Json(nullptr); };
            decisions.write(row(fields(prefix, {trial.jobId, telemetry::milliseconds(event.elapsedNs), measured(event.load.submitted),
                measured(event.load.completed), measured(event.load.bytes), measured(event.load.submittedIops), measured(event.load.completedIops),
                observed && event.load.p95Ms ? Json(*event.load.p95Ms) : Json(nullptr), measured(event.load.outstanding),
                measured(event.decision.smoothedPressure), scheduling::name(event.decision.pressure), event.decision.delayUs / 1000.0,
                event.decision.reason, event.completedBlocks, !event.load.p95Ms.has_value(), event.controllerNs, "MEASURED"})));
            continue;
        }
        processing += event.processingNs;
        locking += event.lockNs;
        wait += event.waitNs;
        checkpoint += event.checkpointNs;
        blocks = event.completedBlocks;
        totalBlocks = event.totalBlocks;
        readBlocks += event.physical.read.blocks;
        writeBlocks += event.physical.write.blocks;
        readRequests += event.physical.read.attempted;
        writeRequests += event.physical.write.attempted;
        progress.write(row(fields(prefix, {trial.jobId, telemetry::milliseconds(event.elapsedNs), blocks, totalBlocks,
            stage(event.kind), telemetry::milliseconds(event.processingNs), telemetry::milliseconds(event.lockNs),
            telemetry::milliseconds(event.waitNs), telemetry::milliseconds(event.checkpointNs),
            event.physical.read.attempted, event.physical.write.attempted, event.physical.read.blocks,
            event.physical.write.blocks, event.load.outstanding, "MEASURED"})));
    }
    if (previous && !trial.traces.empty() && trial.traces.back().elapsedNs >= previous->elapsedNs)
        residency[previous->decision.pressure] += trial.traces.back().elapsedNs - previous->elapsedNs;
    const auto stats = telemetry::summarize(std::move(response));
    const auto rebuildStats = telemetry::summarize(std::move(active));
    const auto waitStats = telemetry::summarize(std::move(queue));
    const auto readStats = telemetry::summarize(std::move(operation));
    const auto as = [](const auto& data, double telemetry::Latency::*value) { return data ? Json((*data).*value) : Json(nullptr); };
    const auto windowSeconds = static_cast<double>(trial.windowNs) / 1e9;
    const auto exposure = trial.rebuildNs ? static_cast<double>(std::min(trial.windowNs, *trial.rebuildNs)) / 1e9 : 0;
    const auto rebuildSeconds = trial.rebuildNs ? static_cast<double>(*trial.rebuildNs) / 1e9 : 0;
    const auto hasWorker = prefix[4] != "DEGRADED_ONLY";
    const auto worker = [&](auto value) { return hasWorker ? Json(value) : Json(nullptr); };
    const auto seconds = [](std::uint64_t value) { return static_cast<double>(value) / 1e9; };
    summaries.write(row(fields(prefix, {study.seed, profile.phases.size() == 1 ? Json(profile.phases[0].rate) : Json(nullptr), trial.windowNs,
        trial.reads.size(), submitted, completed, failed, dropped, rejected, within, within / windowSeconds,
        within * static_cast<double>(raidBlockSize) / mib / windowSeconds,
        as(stats, &telemetry::Latency::meanMs), as(stats, &telemetry::Latency::p50Ms), as(stats, &telemetry::Latency::p95Ms),
        as(stats, &telemetry::Latency::p99Ms), as(waitStats, &telemetry::Latency::meanMs), as(waitStats, &telemetry::Latency::p95Ms),
        as(readStats, &telemetry::Latency::p95Ms), rebuildStats ? Json(rebuildStats->p95Ms) : Json(nullptr),
        as(rebuildStats, &telemetry::Latency::p99Ms), std::count_if(trial.reads.begin(), trial.reads.end(),
            [](const auto& sample) { return sample.active && sample.success; }), activeCompleted,
        exposure ? Json(exposure) : Json(nullptr), exposure ? Json(activeCompleted / exposure) : Json(nullptr),
        rebuildSeconds ? Json(rebuildSeconds) : Json(nullptr), worker(seconds(processing)), worker(seconds(locking)),
        worker(seconds(wait)), worker(seconds(checkpoint)), rebuildSeconds ? Json(seconds(locking) / rebuildSeconds) : Json(nullptr),
        worker(blocks), worker(totalBlocks), worker(readRequests), worker(writeRequests), worker(readBlocks), worker(writeBlocks),
        rebuildSeconds ? Json(blocks * static_cast<double>(raidBlockSize) / mib / rebuildSeconds) : Json(nullptr),
        worker(updates), worker(transitions), worker(validReadSignal), worker(telemetry::milliseconds(overhead)),
        updates ? Json(static_cast<double>(sumDelay) / updates / 1000.0) : Json(nullptr),
        updates ? Json(minDelay / 1000.0) : Json(nullptr), updates ? Json(maxDelay / 1000.0) : Json(nullptr),
        worker(seconds(residency[scheduling::Pressure::Idle])), worker(seconds(residency[scheduling::Pressure::Low])),
        worker(seconds(residency[scheduling::Pressure::Medium])), worker(seconds(residency[scheduling::Pressure::High])),
        worker(seconds(residency[scheduling::Pressure::Warming])),
        trial.jobState, trial.health, trial.foregroundValidated, hasWorker ? Json(trial.rebuildValidated) : Json(nullptr),
        trial.sourceUnchanged, trial.passed ? "COMPLETED" : "FAILED", trial.error, "MEASURED"})));
}

}  // namespace

RunResult runStudy(const Study& study, const fs::path& root) {
    const auto outputParent = outputRoot(root, {});
    const auto workspaceParent = root / "workspaces";
    requireDirectory(outputParent);
    requireDirectory(workspaceParent);
    auto lock = lockWorkspace(workspaceParent);
    const auto space = fs::space(workspaceParent).available;
    if (space < (study.diskMiB * (study.members + 1) + 256) * mib)
        throw std::runtime_error("Insufficient space for bounded rebuild study");
    const auto id = "rebuild-" + newRunId().substr(4);
    const auto output = outputParent / id;
    const auto workspace = workspaceParent / id;
    if (!fs::create_directory(output) || !fs::create_directory(workspace))
        throw std::runtime_error("Rebuild study identifier already exists");
    guardPath(output);
    guardPath(workspace);
    ResultFile foreground(output / "foreground_requests.csv", 64 * mib);
    ResultFile progress(output / "rebuild_progress.csv", 96 * mib);
    ResultFile decisions(output / "scheduler_decisions.csv", 48 * mib);
    ResultFile summaries(output / "policy_results.csv");
    constexpr std::string_view prefix = "schema_version,run_id,trial,repetition,policy,foreground_load,raid_level,";
    for (auto* file : {&foreground, &progress, &decisions, &summaries}) file->write(prefix);
    foreground.write("sequence,scheduled_arrival_ns,actual_submit_ns,operation_start_ns,operation_end_ns,completion_ns,queue_wait_ns,"
        "operation_duration_ns,response_latency_ns,logical_lba,logical_blocks,success,validation_passed,rebuild_active,error,source\n");
    progress.write("job_id,elapsed_ms,completed_blocks,total_blocks,worker_state,batch_duration_ms,catalog_hold_ms,throttle_wait_ms,"
        "checkpoint_ms,physical_read_requests,physical_write_requests,physical_read_blocks,physical_write_blocks,foreground_active_requests,source\n");
    decisions.write("job_id,control_time_ms,foreground_submitted,foreground_completed,foreground_bytes,foreground_arrival_iops,"
        "foreground_completed_iops,foreground_p95_ms,foreground_outstanding,smoothed_pressure,pressure_state,configured_delay_ms,"
        "decision_reason,completed_rebuild_blocks,missing_latency,controller_ns,source\n");
    summaries.write("seed,offered_iops,window_ns,foreground_scheduled,foreground_submitted,foreground_completed,foreground_failed,"
        "foreground_dropped,validation_failures,foreground_within_window,foreground_iops,foreground_throughput_mib_s,"
        "foreground_mean_ms,foreground_p50_ms,foreground_p95_ms,foreground_p99_ms,queue_mean_ms,queue_p95_ms,operation_p95_ms,"
        "active_foreground_p95_ms,active_foreground_p99_ms,active_foreground_completed,active_window_completed,active_exposure_seconds,"
        "active_iops,rebuild_elapsed_seconds,rebuild_processing_seconds,catalog_hold_seconds,rebuild_throttle_wait_seconds,checkpoint_seconds,"
        "catalog_duty_cycle,rebuild_blocks_completed,rebuild_total_blocks,rebuild_physical_read_requests,rebuild_physical_write_requests,"
        "rebuild_physical_read_blocks,rebuild_physical_write_blocks,rebuild_mib_s,controller_updates,state_transitions,valid_latency_updates,"
        "controller_total_ms,mean_configured_delay_ms,min_configured_delay_ms,max_configured_delay_ms,idle_seconds,low_seconds,medium_seconds,"
        "high_seconds,warming_seconds,job_state,final_raid_health,foreground_validation_passed,rebuild_validation_passed,"
        "source_images_unchanged,status,error,source\n");

    const auto started = utc("%Y-%m-%dT%H:%M:%SZ");
    const auto deadline = Clock::now() + std::chrono::seconds(study.runtimeSeconds);
    auto order = Json::array();
    std::uint64_t number = 0;
    bool passed = true;
    for (std::uint64_t rep = 1; rep <= study.repetitions; ++rep) {
        for (std::size_t j = 0; j < study.profiles.size(); ++j) {
            const auto index = (j + rep - 1) % study.profiles.size();
            const auto& profile = study.profiles[index];
            for (std::size_t i = 0; i < study.conditions.size(); ++i) {
                const auto& condition = study.conditions[(i + rep - 1 + index) % study.conditions.size()];
                if (condition == "DEGRADED_ONLY" &&
                    std::none_of(profile.phases.begin(), profile.phases.end(), [](const auto& phase) { return phase.rate > 0; })) continue;
                ++number;
                const auto trialRoot = workspace / ("trial-" + std::to_string(number));
                if (!fs::create_directory(trialRoot)) throw std::runtime_error("Study trial workspace already exists");
                auto trial = runTrial(study, condition, profile, rep, trialRoot,
                    std::min(deadline, Clock::now() + std::chrono::seconds(30)));
                try { removeWorkspace(workspace, trialRoot); }
                catch (const std::exception& exc) { trial.passed = false; if (trial.error.empty()) trial.error = exc.what(); }
                passed = passed && trial.passed;
                writeTrial(foreground, progress, decisions, summaries,
                    Json::array({1, id, number, rep, condition, profile.id, static_cast<int>(study.level)}), trial, profile, study);
                order.push_back({{"trial", number}, {"repetition", rep}, {"policy", condition},
                                 {"foreground_load", profile.id}, {"status", trial.passed ? "COMPLETED" : "FAILED"}});
                std::cerr << "Trial " << number << ' ' << condition << ' ' << profile.id
                          << (trial.passed ? " completed" : " failed: " + trial.error) << '\n';
            }
        }
    }
    if (fs::is_empty(workspace)) fs::remove(workspace);
    for (auto* file : {&foreground, &progress, &decisions, &summaries}) file->flush();
    const Json manifest{{"schema_version", 1}, {"source", "MEASURED"}, {"run_id", id}, {"configuration", study.normalized},
        {"status", passed ? "COMPLETED" : "FAILED"}, {"host", host()}, {"build_type", BENCHMARK_BUILD_TYPE},
        {"compiler", BENCHMARK_COMPILER}, {"cmake_version", BENCHMARK_CMAKE}, {"controller", "BOUNDED_EWMA_1"},
        {"started_at_utc", started}, {"finished_at_utc", utc("%Y-%m-%dT%H:%M:%SZ")}, {"execution_order", order},
        {"cache", "HOST_CACHE_ENABLED_PREPARED"}, {"failed_slot", 0}, {"foreground_workers", 1},
        {"request_bytes", raidBlockSize}, {"member_count", study.members},
        {"stripe_unit_blocks", study.level == RaidLevel::Raid1 ? 1 : 16},
        {"files", {"foreground_requests.csv", "rebuild_progress.csv", "scheduler_decisions.csv", "policy_results.csv"}}};
    ResultFile destination(output / "manifest.json");
    destination.write(manifest.dump(2) + "\n");
    destination.flush();
    return {output, passed};
}

}  // namespace raid::benchmark
