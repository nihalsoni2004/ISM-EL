#include "ipc/operations.h"
#include "ipc/telemetry_api.h"

#include "core/engine.h"
#include "files/file_manager.h"
#include "provisioning/catalog.h"
#include "raid/raid5.h"
#include "raid/raid6.h"
#include "recovery/rebuild_manager.h"
#include "recovery/scheduling/json.h"
#include "telemetry/telemetry.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>

namespace raid::ipc {
namespace {

using namespace std::string_view_literals;
namespace fs = std::filesystem;
constexpr std::uint64_t mib = 1024 * 1024;
constexpr std::uint64_t maxLba = std::numeric_limits<std::uint64_t>::max() / raidBlockSize;

class Params {
public:
    explicit Params(const Json& value) : values_(value) {}

    void only(std::initializer_list<std::string_view> names) const {
        for (const auto& item : values_.items()) {
            if (std::find(names.begin(), names.end(), item.key()) == names.end()) {
                throw Error("INVALID_PARAMS", "Unexpected operation parameter.");
            }
        }
    }

    bool contains(const char* key) const { return values_.contains(key); }

    bool boolean(const char* key) const {
        const auto& value = get(key);
        if (!value.is_boolean()) invalid(key);
        return value.get<bool>();
    }

    double number(const char* key, double minimum, double maximum) const {
        const auto& value = get(key);
        if (!value.is_number()) invalid(key);
        const auto result = value.get<double>();
        if (!std::isfinite(result) || result < minimum || result > maximum) invalid(key);
        return result;
    }

    const Json& object(const char* key) const {
        const auto& value = get(key);
        if (!value.is_object()) invalid(key);
        return value;
    }

    std::string text(const char* key, std::size_t limit = 64) const {
        const auto& value = get(key);
        if (!value.is_string() || value.get_ref<const std::string&>().empty() ||
            value.get_ref<const std::string&>().size() > limit ||
            value.get_ref<const std::string&>().find('\0') != std::string::npos) {
            invalid(key);
        }
        return value.get<std::string>();
    }

    std::string id(const char* key, std::string_view prefix) const {
        auto value = text(key);
        if (!provisioning::validId(value, prefix) || (prefix == "rebuild" && !isRebuildId(value))) {
            invalid(key);
        }
        return value;
    }

    std::uint64_t integer(const char* key, std::uint64_t max, std::uint64_t min = 0) const {
        const auto& value = get(key);
        if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0)) {
            invalid(key);
        }
        const auto number = value.get<std::uint64_t>();
        if (number < min || number > max) {
            invalid(key);
        }
        return number;
    }

    const Json& list(const char* key, std::size_t minimum = 2, std::size_t maximum = 257) const {
        const auto& value = get(key);
        if (!value.is_array() || value.size() < minimum || value.size() > maximum) {
            invalid(key);
        }
        return value;
    }

private:
    [[noreturn]] static void invalid(const char* key) {
        throw Error("INVALID_PARAMS", "Invalid or missing parameter: " + std::string(key) + ".");
    }
    const Json& get(const char* key) const {
        const auto found = values_.find(key);
        if (found == values_.end()) {
            invalid(key);
        }
        return *found;
    }
    const Json& values_;
};

template <typename Range, typename Encode>
Json encodeList(const Range& items, Encode encode) {
    auto result = Json::array();
    for (const auto& entry : items) {
        result.push_back(encode(entry));
    }
    return result;
}

Json optionalNumber(std::optional<std::uint64_t> value) {
    return value ? Json(*value) : Json(nullptr);
}

Json disk(const VirtualDiskInfo& info) {
    return {{"disk_id", info.id}, {"capacity_bytes", info.capacityBytes},
            {"block_size", info.blockSize}, {"block_count", info.blockCount}};
}

Json array(const RaidConfiguration& info, RaidHealth health) {
    return {{"array_id", info.id}, {"raid_level", static_cast<int>(info.level)}, {"members", info.members},
            {"stripe_unit_blocks", info.stripeUnitBlocks}, {"capacity_blocks", info.capacityBlocks},
            {"capacity_bytes", info.capacityBlocks * raidBlockSize}, {"block_size", info.blockSize},
            {"layout", info.layout}, {"health", raidHealthName(health)}};
}

Json arrayMembers(const ArrayHealthReport& info) {
    return encodeList(info.members, [](const auto& member) -> Json {
        return {{"disk_id", member.id}, {"state", diskStateName(member.failure.state)},
                {"cause", failureCauseName(member.failure.cause)}, {"present", member.imagePresent},
                {"available", member.available}};
    });
}

Json spare(const SpareListing& info) {
    return {{"disk_id", info.disk.id}, {"capacity_bytes", info.disk.blocks * raidBlockSize},
            {"state", diskStateName(info.state)}, {"assignment", info.assignment.empty() ? Json(nullptr) : Json(info.assignment)},
            {"available", info.available}};
}

Json pool(const PoolInfo& info) {
    const auto chunk = info.options.chunkBlocks * raidBlockSize;
    return {{"pool_id", info.id}, {"array_id", info.arrayId}, {"health", raidHealthName(info.health)},
            {"physical_capacity_bytes", info.totalChunks * chunk}, {"allocated_bytes", info.allocatedChunks * chunk},
            {"reserved_bytes", info.reservedChunks * chunk}, {"free_bytes", info.freeChunks * chunk},
            {"provisioned_bytes", info.provisionedBlocks * raidBlockSize}, {"chunk_bytes", chunk},
            {"capacity_state", capacityStateName(info.capacityState)}, {"total_chunks", info.totalChunks},
            {"warning_percent", info.options.warning}, {"critical_percent", info.options.critical}};
}

Json lun(const LunInfo& info) {
    return {{"lun_id", info.id}, {"pool_id", info.poolId}, {"capacity_bytes", info.blocks * raidBlockSize},
            {"block_count", info.blocks}, {"chunk_blocks", info.chunkBlocks}, {"allocated_chunks", info.allocatedChunks},
            {"mode", info.fileManaged ? "file" : "raw"}};
}

Json file(const FileRecord& info) {
    return {{"file_id", "file" + std::to_string(info.id)}, {"lun_id", info.lun}, {"name", info.name},
            {"size_bytes", info.size}, {"start_lba", info.startLba}, {"block_count", info.blocks},
            {"sha256", info.sha256.empty() ? Json(nullptr) : Json(info.sha256)}, {"status", info.status},
            {"created_at", info.created}, {"updated_at", info.updated}};
}

Json verification(const FileVerification& value) {
    const auto code = value.passed() ? Json(nullptr) :
        Json(storageError("file.verify", std::runtime_error(value.error)).code);
    return {{"file_id", "file" + std::to_string(value.id)}, {"name", value.name}, {"size_bytes", value.size},
            {"expected_sha256", value.expected}, {"actual_sha256", value.actual.empty() ? Json(nullptr) : Json(value.actual)},
            {"passed", value.passed()}, {"error_code", code}};
}

std::uint64_t fileId(const Params& p) {
    auto id = p.id("file_id", "file");
    std::uint64_t number = 0;
    for (const char ch : std::string_view(id).substr(4)) {
        number = number * 10 + static_cast<unsigned>(ch - '0');
    }
    if (number == 0 || number > INT64_MAX) {
        throw Error("INVALID_PARAMS", "Invalid file identifier.");
    }
    return number;
}

bool lowerHex(char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
}

std::string exchangeToken(const Params& p) {
    auto token = p.text("token", 32);
    if (token.size() != 32 || !std::all_of(token.begin(), token.end(), lowerHex)) {
        throw Error("INVALID_PARAMS", "Invalid exchange token.");
    }
    return token;
}

std::string uploadName(const Params& p) {
    auto name = p.text("name", 240);
    if (name.find_first_of("<>:\"/\\|?*") != std::string::npos || name.back() == '.' || name.back() == ' ' ||
        std::any_of(name.begin(), name.end(), [](unsigned char ch) { return ch < 32 || ch == 127; })) {
        throw Error("INVALID_PARAMS", "Expected a safe upload basename.");
    }
    auto stem = name.substr(0, name.find('.'));
    while (!stem.empty() && stem.back() == ' ') {
        stem.pop_back();
    }
    std::transform(stem.begin(), stem.end(), stem.begin(), [](char ch) {
        return ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch;
    });
    const auto number = std::string_view(stem).substr(std::min<std::size_t>(3, stem.size()));
    const bool deviceNumber = (number.size() == 1 && number[0] >= '0' && number[0] <= '9') ||
        number == "\xc2\xb9" || number == "\xc2\xb2" || number == "\xc2\xb3";
    if (stem == "CON" || stem == "NUL" || stem == "PRN" || stem == "AUX" ||
        stem == "CONIN$" || stem == "CONOUT$" ||
        (deviceNumber && (stem.substr(0, 3) == "COM" || stem.substr(0, 3) == "LPT"))) {
        throw Error("INVALID_PARAMS", "Expected a safe upload basename.");
    }
    return name;
}

using Block = std::array<std::uint8_t, raidBlockSize>;
constexpr std::string_view hex = "0123456789abcdef";

Block decodeBlock(const Params& p) {
    const auto encoded = p.text("data_hex", 2 * raidBlockSize);
    if (encoded.size() != 2 * raidBlockSize || !std::all_of(encoded.begin(), encoded.end(), lowerHex)) {
        throw Error("INVALID_PARAMS", "data_hex must encode exactly one 4096-byte block.");
    }
    Block result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>(16 * hex.find(encoded[2 * index]) + hex.find(encoded[2 * index + 1]));
    }
    return result;
}

std::string encodeBlock(const Block& block) {
    std::string result;
    result.reserve(block.size() * 2);
    for (const auto byte : block) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

Json raidOperation(RaidArrayManager& arrays, std::string_view action, const Params& p) {
    if (action == "list") {
        p.only({});
        return encodeList(arrays.listArraysWithHealth(), [](const auto& entry) {
            return array(entry.configuration, entry.health);
        });
    }
    if (action == "create") {
        p.only({"array_id", "raid_level", "members", "stripe_unit_blocks"});
        const auto id = p.id("array_id", "array");
        const auto value = p.integer("raid_level", 6);
        if (value != 0 && value != 1 && value != 5 && value != 6) {
            throw Error("INVALID_RAID_CONFIGURATION", "Supported RAID levels are 0, 1, 5 and 6.");
        }
        std::vector<std::string> members;
        for (const auto& member : p.list("members")) {
            if (!member.is_string() || !VirtualDiskManager::isValidDiskId(member.get_ref<const std::string&>())) {
                throw Error("INVALID_RAID_CONFIGURATION", "Invalid member disk identifier.");
            }
            members.push_back(member.get<std::string>());
        }
        const auto stripe = p.contains("stripe_unit_blocks") ? p.integer("stripe_unit_blocks", maxLba, 1)
            : value == 1 ? mirrorStripeUnitBlocks : defaultStripeUnitBlocks;
        auto& controller = arrays.createArray(id, static_cast<RaidLevel>(value), std::move(members), stripe);
        return array(controller.configuration(), controller.status());
    }
    if (action == "fail" || action == "info" || action == "status" || action == "members" ||
        action == "verify" || action == "map") {
        const auto id = p.id("array_id", "array");
        if (action == "fail" || action == "status" || action == "members") {
            if (action == "fail") {
                p.only({"array_id", "disk_id"});
                arrays.failMember(id, p.id("disk_id", "disk"));
            } else {
                p.only({"array_id"});
            }
            auto report = arrays.getArrayStatus(id);
            if (action == "members") {
                return arrayMembers(report);
            }
            return {{"array_id", id}, {"raid_level", static_cast<int>(report.level)},
                    {"health", raidHealthName(report.health)}, {"pending_write", report.pendingWrite},
                    {"reads_permitted", readsPermitted(report.health)},
                    {"writes_permitted", writesPermitted(report.health) && !report.poolOwned},
                    {"pool_owned", report.poolOwned}, {"tolerated_failures", report.toleratedFailures}};
        }
        if (action == "map") {
            p.only({"array_id", "lba"});
        } else {
            p.only({"array_id"});
        }
        auto& controller = arrays.openArray(id);
        const auto& config = controller.configuration();
        if (action == "info") {
            return array(config, controller.status());
        }
        if (action == "verify") {
            controller.verify();
            return {{"array_id", id}, {"passed", true}};
        }
        const auto lba = p.integer("lba", maxLba);
        auto sites = controller.map(lba);
        if (config.level == RaidLevel::Raid5) {
            sites.push_back(static_cast<const Raid5&>(controller).inspectMapping(lba).parity);
        } else if (config.level == RaidLevel::Raid6) {
            const auto layout = static_cast<const Raid6&>(controller).inspectMapping(lba);
            sites.push_back(layout.p);
            sites.push_back(layout.q);
        }
        auto positions = Json::array();
        for (std::size_t slot = 0; slot < sites.size(); ++slot) {
            const auto& item = sites[slot];
            const auto role = config.level == RaidLevel::Raid1 ? "mirror" :
                slot == 0 ? "data" : slot == 1 ? "p" : "q";
            positions.push_back({{"disk_id", config.members.at(item.memberIndex)}, {"role", role},
                                 {"physical_lba", item.physicalLba}});
        }
        return {{"array_id", id}, {"lba", lba}, {"locations", std::move(positions)}};
    }
    throw Error("UNKNOWN_OPERATION", "Operation is not supported.");
}

Json parseModelScenario(const Params& input) {
    using namespace performance;
    input.only({"profile", "parameters", "devices", "workload"});
    const auto& selected = profile(input.text("profile"));
    auto settings = selected.settings;
    if (input.contains("parameters")) {
        const Params custom(input.object("parameters"));
        custom.only({"sequential_read_ms", "sequential_write_ms", "random_read_ms", "random_write_ms",
                     "read_mib_per_second", "write_mib_per_second", "queue_depth"});
        settings = {custom.number("sequential_read_ms", 0, maxLatencyMs), custom.number("sequential_write_ms", 0, maxLatencyMs),
                    custom.number("random_read_ms", 0, maxLatencyMs), custom.number("random_write_ms", 0, maxLatencyMs),
                    custom.number("read_mib_per_second", minBandwidth, maxBandwidth),
                    custom.number("write_mib_per_second", minBandwidth, maxBandwidth),
                    static_cast<std::size_t>(custom.integer("queue_depth", maxQueueDepth, 1))};
    }
    const auto devices = static_cast<std::size_t>(input.integer("devices", maxDevices, 1));
    std::vector<Request> requests;
    for (const auto& item : input.list("workload", 1, maxRequests)) {
        if (!item.is_object()) throw Error("INVALID_PARAMS", "Each modeled request must be an object.");
        const Params row(item);
        row.only({"device", "arrival_ms", "operation", "bytes", "access", "depends_on"});
        const auto operation = row.text("operation");
        const auto access = row.text("access");
        if ((operation != "READ" && operation != "WRITE") || (access != "RANDOM" && access != "SEQUENTIAL")) {
            throw Error("INVALID_PARAMS", "Unsupported modeled operation or access pattern.");
        }
        Request request{static_cast<std::size_t>(row.integer("device", devices - 1)), row.number("arrival_ms", 0, maxArrivalMs),
            operation == "READ" ? Operation::Read : Operation::Write, row.integer("bytes", maxBytes, 1),
            access == "RANDOM" ? Access::Random : Access::Sequential, {}};
        if (row.contains("depends_on")) {
            for (const auto& dependency : row.list("depends_on", 0, maxDependencies)) {
                if (!dependency.is_number_integer() || (!dependency.is_number_unsigned() && dependency.get<std::int64_t>() < 0) ||
                    dependency.get<std::uint64_t>() >= requests.size()) {
                    throw Error("INVALID_PARAMS", "A dependency must name an earlier request.");
                }
                request.dependencies.push_back(dependency.get<std::size_t>());
            }
        }
        requests.push_back(std::move(request));
    }
    return modelSimulation(selected, settings, devices, requests);
}

}  // namespace

Error storageError(std::string_view operation, const std::exception& error) {
    const std::string_view what = error.what();
    const auto has = [&](std::string_view part) { return what.find(part) != std::string_view::npos; };
    if (has(".catalog.lock") || has("SQLite catalog busy") || has("Lock storage record")) {
        return {"STORAGE_LOCKED", "Storage catalog could not be locked."};
    }
    if (has("out of space") || has("Insufficient logical") || has("There is not enough space")) {
        return {"OUT_OF_SPACE", "Insufficient storage capacity."};
    }
    if (has("SHA-256 mismatch")) {
        return {"CHECKSUM_MISMATCH", "Stored content failed SHA-256 verification."};
    }
    if (has("DEGRADED") || has("healthy array") || has("requires both members")) {
        return {"ARRAY_DEGRADED", "Operation requires a healthy array."};
    }
    if (has("INCONSISTENT") || has("is inconsistent")) {
        return {"ARRAY_INCONSISTENT", "The array has an unresolved consistency error."};
    }
    if (has("is FAILED") || has("no readable member")) {
        return {"ARRAY_FAILED", "The array cannot serve this operation."};
    }
    if (has("Raw LUN write rejected") || has("file-managed by this") || has("Only completed files")) {
        return {"INVALID_RESOURCE_STATE", "The resource does not allow this operation."};
    }
    if (has("already exists") || has("already belongs") || has(" belongs to RAID array ") ||
        has("registered as a spare") || has("already registered") ||
        has("already has") || has("already holds")) {
        return {"DUPLICATE_RESOURCE", "The resource or ownership already exists."};
    }
    if (has("RAID array does not exist")) { return {"ARRAY_NOT_FOUND", "RAID array does not exist."}; }
    if (has("LUN does not exist")) { return {"LUN_NOT_FOUND", "LUN does not exist."}; }
    if (has("Storage pool does not exist")) { return {"POOL_NOT_FOUND", "Storage pool does not exist."}; }
    if (has("File does not exist")) { return {"FILE_NOT_FOUND", "File does not exist."}; }
    if (has("Rebuild job does not exist")) { return {"REBUILD_NOT_FOUND", "Rebuild job does not exist."}; }
    if (has("not a member") || has("already marked") || has("state cannot")) {
        return {"INVALID_MEMBER_STATE", "The member does not allow this operation."};
    }
    if (has("parity mismatch") || has("mirrors differ") || has("P/Q verification") ||
        (operation == "raid.verify" && has("mismatch"))) {
        return {"VERIFICATION_FAILED", "The array failed verification."};
    }
    if (const auto* fsError = dynamic_cast<const fs::filesystem_error*>(&error)) {
        if (fsError->code() == std::errc::no_such_file_or_directory &&
            (operation == "disk.info" || (operation == "raid.create" && has("disk image")))) {
            return {"DISK_NOT_FOUND", "Virtual disk does not exist."};
        }
        if (fsError->code().value() == ERROR_SHARING_VIOLATION || fsError->code().value() == ERROR_LOCK_VIOLATION) {
            return {"STORAGE_LOCKED", "A storage record or image is in use."};
        }
    }
    if ((operation == "file.import" || operation == "file.export") &&
        (has("reparse") || has("without links") || has("file path") || has("Open import source") ||
         has("Check storage file destination"))) {
        return {"STAGING_REJECTED", "The exchange file or directory is unsafe or unavailable."};
    }
    if (dynamic_cast<const std::invalid_argument*>(&error) || dynamic_cast<const std::out_of_range*>(&error)) {
        if (operation == "raid.create") {
            return {"INVALID_RAID_CONFIGURATION", "Invalid RAID configuration or unavailable members."};
        }
        return {"INVALID_PARAMS", "The parameters are invalid for this storage operation."};
    }
    return {"STORAGE_ERROR", "The storage operation failed."};
}

Operations::Operations(fs::path root) : root_(std::move(root)) {
    (void)telemetry::collector();
}
Operations::~Operations() { shutdown(); }

void Operations::collectWorkers() noexcept {
    for (auto it = workers_.begin(); it != workers_.end();) {
        if (it->second.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        try {
            it->second.result.get();
        } catch (const std::exception& error) {
            lastWorkerFailure_ = it->first;
            std::cerr << "Rebuild worker " << it->first << ": " << error.what() << '\n';
        }
        it = workers_.erase(it);
    }
}

void Operations::shutdown() noexcept {
    for (auto& worker : workers_) {
        worker.second.stop->store(true);
    }
    for (auto& worker : workers_) {
        worker.second.result.wait();
    }
    collectWorkers();
}

Json Operations::describeJob(const RebuildJob& job) const {
    const auto worker = workers_.find(job.id);
    const bool completed = job.status == RebuildStatus::Completed;
    const auto percent = completed ? 100.0 : std::min(99.99, 100.0 * static_cast<double>(job.completedBlocks) /
                                                                 static_cast<double>(job.totalBlocks));
    return {{"job_id", job.id}, {"array_id", job.arrayId}, {"failed_disk_id", job.failedMember},
            {"spare_disk_id", job.spareMember}, {"status", rebuildStatusName(job.status)},
            {"completed_blocks", job.completedBlocks}, {"total_blocks", job.totalBlocks}, {"progress_percent", percent},
            {"batch_blocks", job.batchBlocks}, {"started_at", job.started}, {"checkpoint_at", job.checkpoint},
            {"worker_active", worker != workers_.end() || job.status == RebuildStatus::Running},
            {"pause_requested", job.pauseRequested || (worker != workers_.end() && worker->second.stop->load())},
            {"worker_error", lastWorkerFailure_ == job.id || !job.lastError.empty()},
            {"scheduling", scheduling::toJson(job.scheduling)}};
}

Json Operations::rebuild(std::string_view action, const Json& values) {
    Params p(values);
    RebuildManager manager(root_);
    if (action == "list") {
        p.only({});
        return encodeList(manager.list(), [this](const auto& job) { return describeJob(job); });
    }
    if (action == "prepare") {
        p.only({"array_id", "failed_disk_id", "spare_disk_id", "scheduling"});
        scheduling::Config config;
        if (p.contains("scheduling")) {
            try { config = scheduling::fromJson(p.object("scheduling")); }
            catch (const Json::exception&) { throw Error("INVALID_PARAMS", "Invalid rebuild scheduling configuration."); }
        }
        return describeJob(manager.prepare(p.id("array_id", "array"), p.id("failed_disk_id", "disk"),
                                           p.id("spare_disk_id", "disk"), config));
    }
    if (action == "scheduler") {
        p.only({"job_id"});
        const auto job = manager.status(p.id("job_id", "rebuild"));
        auto encoded = Json::array();
        const auto trace = std::find_if(traces_.rbegin(), traces_.rend(), [&](const auto& entry) { return entry.first == job.id; });
        const auto active = workers_.count(job.id) != 0;
        if (trace != traces_.rend()) {
            const auto history = trace->second->recentDecisions();
            const auto from = history.size() > 24 ? history.size() - 24 : 0;
            for (auto i = from; i < history.size(); ++i) {
                const auto& item = history[i];
                const bool observed = job.scheduling.policy == scheduling::Policy::Adaptive;
                encoded.push_back({{"elapsed_ms", telemetry::milliseconds(item.elapsedNs)}, {"delay_us", item.decision.delayUs},
                    {"pressure", scheduling::name(item.decision.pressure)}, {"reason", item.decision.reason},
                    {"submitted_iops", observed ? Json(item.load.submittedIops) : Json(nullptr)},
                    {"completed_iops", observed ? Json(item.load.completedIops) : Json(nullptr)},
                    {"p95_ms", item.load.p95Ms ? Json(*item.load.p95Ms) : Json(nullptr)},
                    {"outstanding", observed ? Json(item.load.outstanding) : Json(nullptr)},
                    {"completed_blocks", item.completedBlocks}});
            }
        }
        const auto last = encoded.empty() ? Json(nullptr) : encoded.back();
        return {{"source", "MEASURED"}, {"job_id", job.id}, {"status", rebuildStatusName(job.status)},
                {"worker_active", active}, {"completed_blocks", job.completedBlocks}, {"total_blocks", job.totalBlocks},
                {"configuration", scheduling::toJson(job.scheduling)}, {"current", active ? last : Json(nullptr)},
                {"decisions", std::move(encoded)}};
    }
    if (action != "status" && action != "pause" && action != "start" && action != "resume") {
        throw Error("UNKNOWN_OPERATION", "Operation is not supported.");
    }
    const auto id = p.id("job_id", "rebuild");
    if (action == "status" || action == "pause") {
        p.only({"job_id"});
        auto job = manager.status(id);
        if (action == "pause") {
            auto worker = workers_.find(id);
            if (worker != workers_.end() && job.status != RebuildStatus::Completed &&
                job.status != RebuildStatus::Failed) {
                worker->second.stop->store(true);
            } else if (job.status == RebuildStatus::Running || job.status == RebuildStatus::Paused) {
                job = manager.pause(id);
            } else {
                throw Error("INVALID_RESOURCE_STATE", "Rebuild job is not running or paused.");
            }
        }
        return describeJob(job);
    }
    p.only({"job_id", "checkpoint_limit"});
    const auto limit = p.contains("checkpoint_limit") ? p.integer("checkpoint_limit", maxLba, 1) : 0;
    auto job = manager.status(id);
    if (workers_.count(id) || job.status == RebuildStatus::Running) {
        throw Error("DUPLICATE_RESOURCE", "A worker is already running this rebuild.");
    }
    if ((action == "start" && job.status != RebuildStatus::Prepared) ||
        (action == "resume" && job.status != RebuildStatus::Paused && job.status != RebuildStatus::Interrupted)) {
        throw Error("INVALID_RESOURCE_STATE", "Rebuild job cannot perform this transition.");
    }
    if (workers_.size() >= 4) {
        throw Error("STORAGE_LOCKED", "All rebuild worker slots are in use.");
    }
    auto stop = std::make_shared<std::atomic_bool>(false);
    auto trace = std::make_shared<scheduling::LiveTrace>();
    auto work = std::async(std::launch::async, [root = root_, id, stop, limit, trace] {
        return RebuildManager(root, 16, {}, [trace](const auto& event) { trace->push(event); })
            .run(id, limit, [stop] { return stop->load(); });
    });
    workers_.emplace(id, Worker{std::move(stop), std::move(work)});
    traces_.emplace_back(id, std::move(trace));
    if (traces_.size() > 32) traces_.pop_front();
    if (lastWorkerFailure_ == id) {
        lastWorkerFailure_.clear();
    }
    return describeJob(job);
}

Json Operations::execute(std::string_view operation, const Json& values) {
    collectWorkers();
    const Params p(values);
    if (operation == "rebuild.policies") {
        p.only({});
        auto policies = Json::array();
        for (const auto kind : {scheduling::Policy::FullSpeed, scheduling::Policy::Fixed, scheduling::Policy::Adaptive}) {
            scheduling::Config config;
            config.policy = kind;
            policies.push_back(scheduling::toJson(config));
        }
        return {{"controller_version", "BOUNDED_EWMA_1"}, {"policies", std::move(policies)}};
    }
    if (operation == "telemetry.snapshot") {
        p.only({"include_samples", "event_limit"});
        return telemetrySnapshot(p.contains("include_samples") && p.boolean("include_samples"),
            p.contains("event_limit") ? static_cast<std::size_t>(p.integer("event_limit", telemetry::eventLimit)) : 0);
    }
    if (operation == "telemetry.reset") {
        p.only({});
        return resetTelemetry();
    }
    if (operation == "telemetry.rebuild") {
        p.only({"job_id"});
        const auto job = RebuildManager(root_).status(p.id("job_id", "rebuild"));
        return rebuildTelemetry(job, describeJob(job));
    }
    if (operation == "model.profiles") {
        p.only({});
        return modelProfiles();
    }
    if (operation == "model.simulate") {
        try {
            return parseModelScenario(p);
        } catch (const std::invalid_argument& error) {
            throw Error("INVALID_PARAMS", error.what());
        }
    }
    if (operation == "engine.ping" || operation == "engine.version") {
        p.only({});
        return {{"version", config::applicationVersion}, {"process_id", GetCurrentProcessId()}, {"protocol_version", 1}};
    }
    if (operation == "engine.shutdown") {
        p.only({});
        shutdown();
        return {{"stopped", true}};
    }
    if (operation == "engine.status") {
        p.only({});
        Json result = {{"engine_available", true}, {"catalog_accessible", false}, {"ready", false},
                       {"process_id", GetCurrentProcessId()}, {"version", config::applicationVersion},
                       {"array_health", Json::object()}, {"error_code", nullptr}};
        try {
            StorageEngine engine;
            engine.initialize(root_);
            for (const auto& entry : engine.arrays().listArraysWithHealth()) {
                auto& total = result["array_health"][std::string(raidHealthName(entry.health))];
                total = total.is_null() ? 1 : total.get<std::uint64_t>() + 1;
            }
            result["catalog_accessible"] = result["ready"] = true;
        } catch (const std::exception& error) {
            auto mapped = storageError(operation, error);
            result["error_code"] = mapped.code;
            std::cerr << "Catalog probe: " << error.what() << '\n';
        }
        return result;
    }

    const auto separator = operation.find('.');
    if (separator == std::string_view::npos) {
        throw Error("UNKNOWN_OPERATION", "Operation is not supported.");
    }
    const auto group = operation.substr(0, separator);
    const auto action = operation.substr(separator + 1);
    if (group == "rebuild") {
        return rebuild(action, values);
    }
    if (group == "disk" || group == "raid" || group == "spare") {
        if (operation == "spare.list") {
            p.only({});
            return encodeList(RebuildManager(root_).listSpares(), spare);
        }
        StorageEngine engine;
        engine.initialize(root_);
        if (group == "raid") {
            return raidOperation(engine.arrays(), action, p);
        }
        if (operation == "disk.list") {
            p.only({});
            return encodeList(engine.disks().listDisks(), disk);
        }
        if (operation == "disk.info") {
            p.only({"disk_id"});
            return disk(engine.disks().getDiskInfo(p.id("disk_id", "disk")));
        }
        if (operation == "disk.create") {
            p.only({"disk_id", "capacity_mib"});
            const auto id = p.id("disk_id", "disk");
            const auto bytes = p.integer("capacity_mib", INT64_MAX / mib, 1) * mib;
            engine.arrays().requireUnownedDisk(id);
            engine.disks().createDisk(id, bytes);
            return disk(engine.disks().getDiskInfo(id));
        }
        if (operation == "spare.register") {
            p.only({"disk_id"});
            const auto id = p.id("disk_id", "disk");
            engine.arrays().registerSpare(id);
            return spare({{id, {}, 0, engine.disks().getDiskInfo(id).blockCount}, DiskState::Spare, {}, true});
        }
    }

    if (group == "pool" || group == "lun") {
        ProvisioningManager volumes(root_);
        if (operation == "pool.list") {
            p.only({});
            return encodeList(volumes.listPools(), pool);
        }
        if (operation == "pool.create") {
            p.only({"pool_id", "array_id", "chunk_blocks", "max_ratio", "warning_percent", "critical_percent"});
            PoolOptions options;
            if (p.contains("chunk_blocks")) { options.chunkBlocks = p.integer("chunk_blocks", 1024, 1); }
            if (p.contains("max_ratio")) { options.maxProvisioningRatio = p.integer("max_ratio", maxLba); }
            if (p.contains("warning_percent")) { options.warning = p.integer("warning_percent", 99, 1); }
            if (p.contains("critical_percent")) { options.critical = p.integer("critical_percent", 100, 2); }
            return pool(volumes.createPool(p.id("pool_id", "pool"), p.id("array_id", "array"), options));
        }
        if (operation == "pool.info" || operation == "pool.verify") {
            p.only({"pool_id"});
            const auto id = p.id("pool_id", "pool");
            auto result = pool(action == "verify" ? volumes.verifyPool(id) : volumes.poolInfo(id));
            if (action == "verify") { result["passed"] = true; }
            return result;
        }
        if (operation == "lun.list") {
            p.only({});
            return encodeList(volumes.listLuns(), lun);
        }
        if (operation == "lun.info") {
            p.only({"lun_id"});
            return lun(volumes.lunInfo(p.id("lun_id", "lun")));
        }
        if (operation == "lun.create") {
            p.only({"lun_id", "pool_id", "capacity_mib", "mode"});
            const auto id = p.id("lun_id", "lun");
            const auto poolId = p.id("pool_id", "pool");
            const auto bytes = p.integer("capacity_mib", std::numeric_limits<std::uint64_t>::max() / mib, 1) * mib;
            const auto mode = p.contains("mode") ? p.text("mode") : "raw";
            if (mode == "raw") { return lun(volumes.createLun(id, poolId, bytes)); }
            if (mode == "file") { return lun(FileManager(root_).createLun(id, poolId, bytes)); }
            throw Error("INVALID_PARAMS", "LUN mode must be raw or file.");
        }
        if (operation == "lun.map" || operation == "lun.read" || operation == "lun.write") {
            p.only(action == "write" ? std::initializer_list<std::string_view>{"lun_id", "lba", "data_hex"}
                                     : std::initializer_list<std::string_view>{"lun_id", "lba"});
            const auto id = p.id("lun_id", "lun");
            const auto lba = p.integer("lba", maxLba);
            if (action == "map") {
                const auto mapping = volumes.map(id, lba);
                return {{"lun_id", id}, {"lba", lba}, {"logical_chunk", mapping.logicalChunk},
                        {"offset", mapping.offset}, {"physical_chunk", optionalNumber(mapping.physicalChunk)},
                        {"raid_lba", optionalNumber(mapping.raidLba)}};
            }
            if (volumes.lunInfo(id).fileManaged) {
                throw Error("INVALID_RESOURCE_STATE", "Raw block access requires a raw-mode LUN.");
            }
            if (action == "write") {
                const auto content = decodeBlock(p);
                const auto written = volumes.writeBlocks(id, lba, 1, content.data(), content.size());
                return {{"lun_id", id}, {"lba", lba}, {"new_chunks", written.newChunks},
                        {"allocated_chunks", written.allocatedChunks}};
            }
            Block content{};
            const auto mapping = volumes.readBlocks(id, lba, 1, content.data(), content.size());
            return {{"lun_id", id}, {"lba", lba}, {"mapped", mapping.physicalChunk.has_value()},
                    {"data_hex", encodeBlock(content)}};
        }
    }

    if (group == "file") {
        FileManager files(root_);
        if (operation == "file.list") {
            p.only({"lun_id"});
            return encodeList(files.list(p.contains("lun_id") ? p.id("lun_id", "lun") : std::string{}), file);
        }
        if (operation == "file.info") {
            p.only({"file_id"});
            return file(files.info(fileId(p)));
        }
        if (operation == "file.verify_all") {
            p.only({"lun_id"});
            return encodeList(files.verifyAll(p.id("lun_id", "lun")), verification);
        }
        if (operation == "file.verify" || operation == "file.export") {
            p.only(action == "export" ? std::initializer_list<std::string_view>{"file_id", "token"}
                                      : std::initializer_list<std::string_view>{"file_id"});
            const auto id = fileId(p);
            const auto result = action == "export"
                ? files.exportFile(id, root_ / "ipc_exchange" / "downloads" / exchangeToken(p) / "payload.bin")
                : files.verify(id);
            if (!result.passed()) {
                throw storageError(operation, std::runtime_error(result.error));
            }
            return verification(result);
        }
        if (operation == "file.import") {
            p.only({"lun_id", "token", "name"});
            const auto path = root_ / "ipc_exchange" / "uploads" / exchangeToken(p) / fs::u8path(uploadName(p));
            return file(files.importFile(p.id("lun_id", "lun"), path));
        }
    }
    throw Error("UNKNOWN_OPERATION", "Operation is not supported.");
}

}  // namespace raid::ipc
