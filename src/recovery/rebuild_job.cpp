#include "recovery/rebuild_job.h"

#include "disk/virtual_disk_manager.h"
#include "raid/raid_array_manager.h"
#include "record.h"

#include <algorithm>
#include <set>

namespace raid {

std::string_view rebuildStatusName(RebuildStatus status) noexcept {
    switch (status) {
    case RebuildStatus::Prepared: return "PREPARED";
    case RebuildStatus::Running: return "RUNNING";
    case RebuildStatus::Paused: return "PAUSED";
    case RebuildStatus::Interrupted: return "INTERRUPTED";
    case RebuildStatus::Completed: return "COMPLETED";
    case RebuildStatus::Failed: return "FAILED";
    }
    return "UNKNOWN";
}

bool isRebuildId(std::string_view id) noexcept {
    constexpr std::string_view prefix = "rebuild";
    if (id.size() <= prefix.size() || id.substr(0, prefix.size()) != prefix) {
        return false;
    }
    id.remove_prefix(prefix.size());
    if (id.front() == '0') {
        return false;
    }
    std::uint64_t number = 0;
    const auto end = id.data() + id.size();
    const auto parsed = std::from_chars(id.data(), end, number);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

namespace {

RebuildStatus readStatus(std::string_view text) {
    for (const auto status : {RebuildStatus::Prepared, RebuildStatus::Running, RebuildStatus::Paused,
                              RebuildStatus::Interrupted, RebuildStatus::Completed, RebuildStatus::Failed}) {
        if (text == rebuildStatusName(status)) {
            return status;
        }
    }
    throw std::invalid_argument("Unsupported rebuild job status");
}

void validate(const RebuildJob& job) {
    scheduling::validate(job.scheduling);
    if (!isRebuildId(job.id) || !RaidArrayManager::isValidArrayId(job.arrayId) ||
        !VirtualDiskManager::isValidDiskId(job.spareMember) || !record::identity(job.spareIdentity) ||
        job.slot >= job.members.size() || job.members[job.slot] != job.failedMember ||
        job.members.size() != job.sources.size() || job.sources[job.slot] != "-" ||
        job.level == RaidLevel::Raid0 || job.layout != parityLayout(job.level) ||
        job.batchBlocks == 0 || job.batchBlocks > 1024 || job.completedBlocks > job.totalBlocks ||
        (job.status == RebuildStatus::Completed && job.completedBlocks != job.totalBlocks) ||
        (job.status == RebuildStatus::Prepared && job.completedBlocks != 0)) {
        throw std::invalid_argument("Invalid rebuild job metadata");
    }
    calculateRaidCapacity(job.level, job.members.size(), job.totalBlocks, job.stripe);
    std::set<std::string> unique;
    std::size_t unavailable = 0;
    for (std::size_t index = 0; index < job.members.size(); ++index) {
        const auto& member = job.members[index];
        const auto& source = job.sources[index];
        if (!VirtualDiskManager::isValidDiskId(member) || member == job.spareMember || !unique.insert(member).second) {
            throw std::invalid_argument("Invalid rebuild job member");
        }
        if (source == "-") {
            ++unavailable;
        } else {
            if (source.size() < 66 || source[64] != ':' || !record::identity(std::string_view(source).substr(0, 64))) {
                throw std::invalid_argument("Invalid rebuild source fingerprint");
            }
            record::number(std::string_view(source).substr(65));
        }
    }
    if (unavailable > failureTolerance(job.level)) {
        throw std::invalid_argument("Rebuild job exceeds failure tolerance");
    }
}

}  // namespace

RebuildJobStore::RebuildJobStore(std::filesystem::path directory) : directory_(std::move(directory)) {}

RebuildJob RebuildJobStore::load(std::string_view id) const {
    if (!isRebuildId(id)) {
        throw std::invalid_argument("Invalid rebuild job identifier");
    }
    const auto text = record::read(directory_ / (std::string(id) + ".job"));
    if (!text) {
        throw std::invalid_argument("Rebuild job does not exist: " + std::string(id));
    }
    std::istringstream input(*text);
    const auto schema = record::field(input, "schema_version");
    if ((schema != "1" && schema != "2") || record::field(input, "job_id") != id) {
        throw std::invalid_argument("Unsupported rebuild schema or job ID");
    }
    RebuildJob job;
    job.id = id;
    job.arrayId = record::field(input, "array_id");
    job.failedMember = record::field(input, "failed_member");
    job.spareMember = record::field(input, "spare_member");
    job.spareIdentity = record::field(input, "spare_identity");
    job.slot = record::number(record::field(input, "member_slot"));
    const auto level = record::number(record::field(input, "raid_level"));
    if (level != 1 && level != 5 && level != 6) {
        throw std::invalid_argument("Unsupported RAID level for rebuild");
    }
    job.level = static_cast<RaidLevel>(level);
    job.stripe = record::number(record::field(input, "stripe_unit_blocks"));
    job.layout = record::field(input, "layout");
    job.members = record::split(record::field(input, "members"));
    job.sources = record::split(record::field(input, "sources"));
    job.status = readStatus(record::field(input, "status"));
    job.totalBlocks = record::number(record::field(input, "total_blocks"));
    job.batchBlocks = record::number(record::field(input, "batch_blocks"));
    job.completedBlocks = record::number(record::field(input, "checkpoint_blocks"));
    const auto pause = record::field(input, "pause_requested");
    if (pause != "0" && pause != "1") {
        throw std::invalid_argument("Invalid rebuild pause request");
    }
    job.pauseRequested = pause == "1";
    job.started = record::number(record::field(input, "started"));
    job.checkpoint = record::number(record::field(input, "last_checkpoint"));
    job.lastError = record::field(input, "last_error");
    if (schema == "2") job.scheduling = scheduling::decode(record::field(input, "scheduler"));
    record::requireEnd(input);
    validate(job);
    return job;
}

std::vector<RebuildJob> RebuildJobStore::list() const {
    std::vector<RebuildJob> result;
    for (const auto& file : record::entries(directory_)) {
        if (record::hasExtension(file, L".job")) {
            result.push_back(load(file.stem().u8string()));
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return result;
}

void RebuildJobStore::save(const RebuildJob& job, bool create) const {
    validate(job);
    auto error = job.lastError.substr(0, 4096);
    std::replace(error.begin(), error.end(), '\n', ' ');
    std::replace(error.begin(), error.end(), '\r', ' ');
    std::ostringstream output;
    const auto schedule = scheduling::encode(job.scheduling);
    const bool legacy = schedule == scheduling::encode({});
    output << "schema_version=" << (legacy ? 1 : 2) << "\njob_id=" << job.id << "\narray_id=" << job.arrayId
           << "\nfailed_member=" << job.failedMember << "\nspare_member=" << job.spareMember
           << "\nspare_identity=" << job.spareIdentity << "\nmember_slot=" << job.slot
           << "\nraid_level=" << static_cast<int>(job.level) << "\nstripe_unit_blocks=" << job.stripe
           << "\nlayout=" << job.layout << "\nmembers=" << record::join(job.members)
           << "\nsources=" << record::join(job.sources) << "\nstatus=" << rebuildStatusName(job.status)
           << "\ntotal_blocks=" << job.totalBlocks << "\nbatch_blocks=" << job.batchBlocks
           << "\ncheckpoint_blocks=" << job.completedBlocks
           << "\npause_requested=" << (job.pauseRequested ? 1 : 0) << "\nstarted=" << job.started
           << "\nlast_checkpoint=" << job.checkpoint << "\nlast_error=" << error << '\n';
    if (!legacy) output << "scheduler=" << schedule << '\n';
    disk_file::prepareDirectory(directory_, true);
    const auto path = directory_ / (job.id + ".job");
    if (create) {
        disk_file::publishText(path, output.str());
    } else {
        disk_file::replaceText(path, output.str());
    }
}

std::shared_ptr<void> RebuildJobStore::tryLease(std::string_view id) const {
    if (!isRebuildId(id)) {
        throw std::invalid_argument("Invalid rebuild job identifier");
    }
    return disk_file::lockCatalog(directory_ / (std::string(id) + ".worker.lock"), false);
}

}  // namespace raid
