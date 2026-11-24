#include "recovery/rebuild_manager.h"

#include "core/engine.h"
#include "record.h"

#include <algorithm>
#include <limits>

namespace raid {
namespace {

bool committed(const RaidConfiguration& array, std::string_view id) {
    return std::find(array.completedRebuilds.begin(), array.completedRebuilds.end(), id) !=
           array.completedRebuilds.end();
}

std::vector<std::string> fingerprints(StorageEngine& engine, const RaidController& array) {
    std::vector<std::string> sources;
    const auto& members = array.configuration().members;
    for (std::size_t index = 0; index < members.size(); ++index) {
        if (!array.memberAvailable(index)) {
            sources.emplace_back("-");
        } else {
            const auto& disk = engine.disks().openDisk(members[index]);
            sources.push_back(disk.imageIdentity() + ':' + std::to_string(disk.imageRevision()));
        }
    }
    return sources;
}

}  // namespace

RebuildManager::RebuildManager(std::filesystem::path root, std::uint64_t batchBlocks, PhaseObserver observer, TraceObserver trace)
    : root_(std::move(root)), jobs_(root_ / config::defaultMetadataDirectory / "rebuild_jobs"),
      batchBlocks_(batchBlocks), observer_(std::move(observer)), trace_(std::move(trace)) {
    if (batchBlocks_ == 0 || batchBlocks_ > 1024) {
        throw std::invalid_argument("Rebuild batch size must be between 1 and 1024 blocks");
    }
}

void RebuildManager::observe(RebuildPhase phase) const {
    if (observer_) {
        observer_(phase);
    }
}

RebuildJob RebuildManager::reconcile(RaidArrayManager& arrays, RebuildJob job, bool detectInterruption) const {
    const auto array = arrays.findArray(job.arrayId);
    if (committed(array, job.id)) {
        if (job.status != RebuildStatus::Completed) {
            if (job.completedBlocks != job.totalBlocks) {
                throw std::runtime_error("Promotion has no durable full rebuild checkpoint");
            }
            job.status = RebuildStatus::Completed;
            job.pauseRequested = false;
            job.lastError.clear();
            jobs_.save(job);
        }
        return job;
    }
    if (job.status == RebuildStatus::Completed) {
        throw std::runtime_error("Completed job has no committed replacement");
    }
    if (job.status == RebuildStatus::Running && detectInterruption) {
        const auto lease = jobs_.tryLease(job.id);
        if (lease) {
            job.status = RebuildStatus::Interrupted;
            job.pauseRequested = false;
            job.lastError = "Worker exited before completion; resume from the durable checkpoint.";
            jobs_.save(job);
        }
    }
    return job;
}

RebuildJob RebuildManager::prepare(std::string_view id, std::string_view failed, std::string_view spareId,
                                  scheduling::Config policy) {
    scheduling::validate(policy);
    if (!VirtualDiskManager::isValidDiskId(failed) || !VirtualDiskManager::isValidDiskId(spareId)) {
        throw std::invalid_argument("Invalid failed member or spare identifier");
    }
    StorageEngine engine;
    engine.initialize(root_);
    auto& arrays = engine.arrays();
    auto& array = arrays.openArray(id);
    const auto& configuration = array.configuration();
    if (array.level() == RaidLevel::Raid0 || array.status() != RaidHealth::Degraded) {
        throw std::invalid_argument("Rebuild requires a consistent degraded RAID 1, RAID 5, or RAID 6 array");
    }
    const auto found = std::find(configuration.members.begin(), configuration.members.end(), failed);
    if (found == configuration.members.end()) {
        throw std::invalid_argument("Failed disk is not a member of the array");
    }
    const auto slot = static_cast<std::size_t>(found - configuration.members.begin());
    const auto state = arrays.failures_.load(configuration);
    if (state.members[slot].state != DiskState::Failed || array.memberAvailable(slot)) {
        throw std::invalid_argument("Rebuild target must be a persistently FAILED member");
    }
    const auto spare = arrays.spares_.find(spareId);
    if (!spare) {
        throw std::invalid_argument("Replacement is not a registered spare");
    }

    std::uint64_t highest = 0;
    for (auto job : jobs_.list()) {
        job = reconcile(arrays, std::move(job), true);
        highest = std::max(highest, record::number(std::string_view(job.id).substr(7)));
        if (job.spareMember == spareId) {
            throw std::invalid_argument("Spare already has a rebuild assignment");
        }
        if (job.arrayId == id && job.status != RebuildStatus::Completed && job.status != RebuildStatus::Failed) {
            throw std::invalid_argument("Array already has an unfinished rebuild job");
        }
    }
    for (const auto& item : arrays.loadCatalog()) {
        const auto& members = item.second.members;
        if (std::find(members.begin(), members.end(), spareId) != members.end()) {
            throw std::invalid_argument("Replacement spare is already an active member");
        }
    }
    const auto blocks = array.capacityBlocks() / dataUnitCount(array.level(), configuration.members.size());
    if (spare->blocks != blocks) {
        throw std::invalid_argument("Spare capacity must exactly match the failed member");
    }
    auto& disk = engine.disks().openDisk(spareId);
    SpareManager::validateAvailable(*spare, disk);
    SpareManager::validateBlank(disk);
    if (highest == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Rebuild job ID range exhausted");
    }

    RebuildJob job;
    job.id = "rebuild" + std::to_string(highest + 1);
    job.arrayId = id;
    job.failedMember = failed;
    job.spareMember = spareId;
    job.spareIdentity = spare->identity;
    job.slot = slot;
    job.level = array.level();
    job.stripe = configuration.stripeUnitBlocks;
    job.layout = configuration.layout;
    job.members = configuration.members;
    job.sources = fingerprints(engine, array);
    job.totalBlocks = blocks;
    job.batchBlocks = batchBlocks_;
    job.scheduling = policy;
    jobs_.save(job, true);
    return job;
}

RaidController& RebuildManager::validateSources(StorageEngine& engine, const RebuildJob& job) const {
    auto& array = engine.arrays().openArray(job.arrayId);
    const auto& configuration = array.configuration();
    if (array.status() != RaidHealth::Degraded || array.level() != job.level ||
        configuration.members != job.members || configuration.stripeUnitBlocks != job.stripe ||
        configuration.layout != job.layout ||
        array.capacityBlocks() / dataUnitCount(array.level(), job.members.size()) != job.totalBlocks ||
        fingerprints(engine, array) != job.sources) {
        throw std::runtime_error("Rebuild source availability, identity, revision, or array configuration changed");
    }
    const auto state = engine.arrays().failures_.load(configuration);
    if (state.members[job.slot].state != DiskState::Failed) {
        throw std::runtime_error("Rebuild target is no longer a FAILED slot");
    }
    return array;
}

RebuildJob RebuildManager::status(std::string_view id) {
    StorageEngine engine;
    engine.initialize(root_);
    auto& arrays = engine.arrays();
    arrays.lockCatalog();
    return reconcile(arrays, jobs_.load(id), true);
}

RebuildJob RebuildManager::pause(std::string_view id) {
    StorageEngine engine;
    engine.initialize(root_);
    auto& arrays = engine.arrays();
    arrays.lockCatalog();
    auto job = reconcile(arrays, jobs_.load(id), true);
    if (job.status == RebuildStatus::Paused || (job.status == RebuildStatus::Running && job.pauseRequested)) {
        return job;
    }
    if (job.status != RebuildStatus::Running) {
        throw std::invalid_argument("Pause requires a running rebuild");
    }
    job.pauseRequested = true;
    jobs_.save(job);
    return job;
}

std::vector<RebuildJob> RebuildManager::list() {
    StorageEngine engine;
    engine.initialize(root_);
    auto& arrays = engine.arrays();
    arrays.lockCatalog();
    auto result = jobs_.list();
    for (auto& job : result) {
        job = reconcile(arrays, std::move(job), true);
    }
    return result;
}

std::vector<RebuildJob> RebuildManager::listForArray(RaidArrayManager& arrays, std::string_view id) {
    arrays.lockCatalog();
    std::vector<RebuildJob> result;
    for (auto job : jobs_.list()) {
        if (job.arrayId == id) {
            result.push_back(reconcile(arrays, std::move(job), true));
        }
    }
    return result;
}

std::vector<SpareListing> RebuildManager::listSpares() {
    StorageEngine engine;
    engine.initialize(root_);
    auto& arrays = engine.arrays();
    const auto catalog = arrays.loadCatalog();
    auto jobs = jobs_.list();
    for (auto& job : jobs) {
        job = reconcile(arrays, std::move(job), true);
    }
    std::vector<SpareListing> result;
    for (const auto& spare : arrays.spares_.list()) {
        const auto assigned = std::find_if(jobs.begin(), jobs.end(), [&](const auto& job) {
            return job.spareMember == spare.id;
        });
        const auto active = std::find_if(catalog.begin(), catalog.end(), [&](const auto& entry) {
            const auto& members = entry.second.members;
            return std::find(members.begin(), members.end(), spare.id) != members.end();
        });
        if (active != catalog.end()) {
            const auto report = arrays.inspectArray(active->second);
            const auto member = std::find_if(report.members.begin(), report.members.end(), [&](const auto& item) {
                return item.id == spare.id;
            });
            result.push_back({spare, member->failure.state, assigned == jobs.end() ? "" : assigned->id, false});
            continue;
        }
        if (assigned != jobs.end()) {
            auto state = DiskState::Rebuilding;
            if (assigned->status == RebuildStatus::Prepared) {
                state = DiskState::Spare;
            } else if (assigned->status == RebuildStatus::Completed || assigned->status == RebuildStatus::Failed) {
                state = DiskState::Failed;
            }
            result.push_back({spare, state, assigned->id, false});
            continue;
        }
        bool available = false;
        try {
            const auto info = engine.disks().getDiskInfo(spare.id);
            std::string identity;
            std::uint64_t revision = 0;
            const auto bytes = disk_file::inspect(info.path, &identity, &revision);
            available = bytes / raidBlockSize == spare.blocks && identity == spare.identity && revision == spare.revision;
        } catch (const std::invalid_argument&) {
            // Unsafe or malformed spare image.
        } catch (const std::filesystem::filesystem_error&) {
            // Missing or inaccessible spare image.
        }
        result.push_back({spare, DiskState::Spare, "", available});
    }
    return result;
}

}  // namespace raid
