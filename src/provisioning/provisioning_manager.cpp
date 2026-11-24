#include "provisioning/provisioning_manager.h"

#include "core/engine.h"
#include "disk/image_file.h"
#include "provisioning/catalog.h"
#include "recovery/scheduling/scheduler.h"

#include <algorithm>
#include <limits>
#include <set>

namespace raid {
namespace {

using namespace provisioning;

bool reached(std::uint64_t used, std::uint64_t total, std::uint64_t percent) {
    const auto threshold = (total / 100) * percent + ((total % 100) * percent + 99) / 100;
    return used >= threshold;
}

PoolInfo summary(const Pool& pool, RaidHealth health) {
    std::uint64_t allocated = 0;
    for (const auto& volume : pool.volumes) {
        allocated += volume.second.mappings.size();
    }
    const auto reserved = static_cast<std::uint64_t>(pool.reserved.size());
    const auto used = allocated + reserved;
    auto state = CapacityState::Normal;
    if (used == pool.totalChunks) {
        state = CapacityState::Full;
    } else if (reached(used, pool.totalChunks, pool.options.critical)) {
        state = CapacityState::Critical;
    } else if (reached(used, pool.totalChunks, pool.options.warning)) {
        state = CapacityState::Warning;
    }
    return {pool.id, pool.arrayId, pool.options, pool.raidBlocks, pool.totalChunks, allocated, reserved,
            pool.totalChunks - used, totalProvisioned(pool), state, health};
}

LunInfo summary(const Pool& pool, const std::string& id, const Volume& volume) {
    return {id, pool.id, volume.blocks, pool.options.chunkBlocks, volume.mappings.size(), !volume.fileCatalog.empty()};
}

void checkRange(const Volume& volume, std::uint64_t lba, std::uint64_t count) {
    if (count == 0 || lba >= volume.blocks || count > volume.blocks - lba) {
        throw std::invalid_argument("Logical block range is outside the LUN");
    }
}

void checkTransfer(const Volume& volume, std::uint64_t lba, std::uint64_t count,
                   const void* buffer, std::size_t bytes) {
    checkRange(volume, lba, count);
    if (!buffer || count > bytes / raidBlockSize) {
        throw std::invalid_argument("LUN transfer requires a buffer large enough for the request");
    }
}

LunMapping mappingAt(const Volume& volume, std::uint64_t lba, std::uint64_t chunkBlocks) {
    LunMapping result{lba / chunkBlocks, lba % chunkBlocks, {}, {}};
    const auto mapping = volume.mappings.find(result.logicalChunk);
    if (mapping != volume.mappings.end()) {
        result.physicalChunk = mapping->second;
        result.raidLba = mapping->second * chunkBlocks + result.offset;
    }
    return result;
}

void requireHealth(const RaidController& array, bool writing) {
    const auto health = array.status();
    if (health == RaidHealth::Healthy || (!writing && health == RaidHealth::Degraded)) {
        return;
    }
    throw std::runtime_error("Backing RAID " + array.configuration().id + " is " +
                             std::string(raidHealthName(health)) +
                             (writing ? ". LUN writes are blocked." : ". LUN reads are blocked."));
}

template <typename Transfer>
void chunks(std::uint64_t lba, std::uint64_t count, std::uint64_t chunkBlocks, Transfer transfer) {
    std::size_t ioOffset = 0;
    while (count != 0) {
        const auto chunk = lba / chunkBlocks;
        const auto chunkOffset = lba % chunkBlocks;
        const auto blocks = std::min(count, chunkBlocks - chunkOffset);
        const auto bytes = static_cast<std::size_t>(blocks * raidBlockSize);
        transfer(chunk, chunkOffset, blocks, ioOffset, bytes);
        lba += blocks;
        count -= blocks;
        ioOffset += bytes;
    }
}

void reserveChunks(Pool& pool, const Volume& volume, std::string_view id,
                   std::uint64_t lba, std::uint64_t count) {
    const auto chunkBlocks = pool.options.chunkBlocks;
    const auto first = lba / chunkBlocks;
    const auto last = (lba + count - 1) / chunkBlocks;
    auto missing = last - first + 1;
    for (auto found = volume.mappings.lower_bound(first);
         found != volume.mappings.end() && found->first <= last; ++found) {
        --missing;
    }
    if (missing == 0) {
        return;
    }
    std::set<std::uint64_t> used;
    for (const auto& entry : pool.volumes) {
        for (const auto& mapping : entry.second.mappings) {
            used.insert(mapping.second);
        }
    }
    if (missing > pool.totalChunks - used.size()) {
        throw std::runtime_error("Storage pool is out of space for this write");
    }
    // Bound transaction memory independently of the supplied I/O buffer and pool geometry.
    constexpr std::uint64_t maxNewChunks = 65536;
    if (missing > maxNewChunks) {
        throw std::invalid_argument("A write can allocate at most 65536 new chunks");
    }
    pool.reservedLun = id;
    auto occupied = used.begin();
    std::uint64_t physical = 0;
    for (auto logical = first; logical <= last; ++logical) {
        if (volume.mappings.count(logical)) {
            continue;
        }
        while (occupied != used.end() && *occupied == physical) {
            ++physical;
            ++occupied;
        }
        pool.reserved.emplace(logical, physical++);
    }
}

}  // namespace

struct ProvisioningManager::Session {
    explicit Session(const std::filesystem::path& root) : catalog(lockAndInitialize(root)) {}

    std::string fingerprint(std::string_view arrayId) {
        const auto value = disk_file::textFingerprint(engine.arrays().directory_ / (std::string(arrayId) + ".raid"));
        if (!value) {
            throw std::runtime_error("Backing RAID configuration is missing");
        }
        return *value;
    }

    RaidController& backend(const Pool& pool) {
        if (fingerprint(pool.arrayId) != pool.arrayFingerprint) {
            throw std::runtime_error("Backing RAID configuration identity changed for " + pool.id);
        }
        auto& array = engine.arrays().openArray(pool.arrayId);
        if (!array.poolOwned() || array.capacityBlocks() != pool.raidBlocks ||
            array.configuration().blockSize != raidBlockSize) {
            throw std::runtime_error("Backing RAID ownership or geometry changed for " + pool.id);
        }
        const auto& configuration = array.configuration();
        if (configuration.memberIdentities.empty()) {
            if (configuration.members.size() != pool.legacyIdentities.size()) {
                throw std::runtime_error("Legacy backing RAID member identity count differs for " + pool.id);
            }
            for (std::size_t index = 0; index < configuration.members.size(); ++index) {
                if (array.memberAvailable(index) &&
                    engine.disks().openDisk(configuration.members[index]).imageIdentity() != pool.legacyIdentities[index]) {
                    throw std::runtime_error("Legacy backing RAID member identity changed for " + pool.id);
                }
            }
        }
        return array;
    }

    StorageEngine engine;
    Catalog catalog;

private:
    std::filesystem::path lockAndInitialize(const std::filesystem::path& root) {
        engine.initialize(root);
        auto& arrays = engine.arrays();
        arrays.lockCatalog();
        return arrays.directory_.parent_path() / "provisioning";
    }
};

std::string_view capacityStateName(CapacityState state) noexcept {
    switch (state) {
    case CapacityState::Normal: return "NORMAL";
    case CapacityState::Warning: return "WARNING";
    case CapacityState::Critical: return "CRITICAL";
    case CapacityState::Full: return "FULL";
    }
    return "UNKNOWN";
}

ProvisioningManager::ProvisioningManager(std::filesystem::path root, PhaseObserver observer)
    : root_(std::move(root)), observer_(std::move(observer)) {}

void ProvisioningManager::observe(AllocationPhase phase) const {
    if (observer_) {
        observer_(phase);
    }
}

PoolInfo ProvisioningManager::createPool(std::string_view id, std::string_view arrayId, PoolOptions options) {
    if (!validId(id, "pool")) {
        throw std::invalid_argument("Invalid pool identifier. Use pool0, pool1, and so on.");
    }
    validateOptions(options);
    Session session(root_);
    if (session.catalog.hasPool(id) || session.catalog.ownsArray(arrayId)) {
        throw std::invalid_argument("Pool ID already exists or backing RAID already belongs to a pool");
    }
    auto& array = session.engine.arrays().openArray(arrayId);
    requireHealth(array, true);
    Pool pool;
    pool.id = id;
    pool.arrayId = arrayId;
    pool.options = options;
    pool.raidBlocks = array.capacityBlocks();
    pool.totalChunks = pool.raidBlocks / options.chunkBlocks;
    if (pool.totalChunks == 0) {
        throw std::invalid_argument("Backing RAID capacity cannot fit one complete pool chunk");
    }
    pool.arrayFingerprint = session.fingerprint(arrayId);
    if (array.configuration().memberIdentities.empty()) {
        for (const auto& disk : array.configuration().members) {
            pool.legacyIdentities.push_back(session.engine.disks().openDisk(disk).imageIdentity());
        }
    }
    session.catalog.save(pool, true);
    return summary(pool, array.status());
}

PoolInfo ProvisioningManager::poolInfo(std::string_view id) {
    Session session(root_);
    const auto& pool = session.catalog.pool(id);
    return summary(pool, session.backend(pool).status());
}

std::vector<PoolInfo> ProvisioningManager::listPools() {
    Session session(root_);
    std::vector<PoolInfo> result;
    for (const auto& entry : session.catalog.pools()) {
        result.push_back(summary(entry.second, session.backend(entry.second).status()));
    }
    return result;
}

PoolInfo ProvisioningManager::setWatermarks(std::string_view id, std::uint64_t warning, std::uint64_t critical) {
    Session session(root_);
    auto& pool = session.catalog.pool(id);
    auto& array = session.backend(pool);
    pool.options.warning = warning;
    pool.options.critical = critical;
    validateOptions(pool.options);
    session.catalog.save(pool);
    return summary(pool, array.status());
}

PoolInfo ProvisioningManager::verifyPool(std::string_view id) {
    // Loading verifies IDs, geometry, map and reservation bounds, uniqueness and stored counts.
    return poolInfo(id);
}

LunInfo ProvisioningManager::createLun(std::string_view id, std::string_view poolId, std::uint64_t capacityBytes) {
    return createLunImpl(id, poolId, capacityBytes, {});
}

LunInfo ProvisioningManager::createLunImpl(std::string_view id, std::string_view poolId,
                                            std::uint64_t capacityBytes, std::string_view fileCatalog) {
    if (!validId(id, "lun") || capacityBytes == 0 || capacityBytes % raidBlockSize != 0) {
        throw std::invalid_argument("LUN requires a valid identifier and a positive block-aligned capacity");
    }
    Session session(root_);
    auto& pool = session.catalog.pool(poolId);
    session.backend(pool);
    if (session.catalog.hasLun(id)) {
        throw std::invalid_argument("LUN ID already exists: " + std::string(id));
    }
    const auto blocks = capacityBytes / raidBlockSize;
    const auto total = totalProvisioned(pool);
    if (blocks > std::numeric_limits<std::uint64_t>::max() / raidBlockSize - total) {
        throw std::overflow_error("Total provisioned capacity exceeds the supported byte range");
    }
    if (!withinProvisioningLimit(pool, total + blocks)) {
        throw std::invalid_argument("LUN would exceed the configured pool overprovisioning limit");
    }
    const auto entry = pool.volumes.emplace(std::string(id), Volume{blocks, {}, std::string(fileCatalog)}).first;
    session.catalog.save(pool);
    return summary(pool, entry->first, entry->second);
}

LunInfo ProvisioningManager::lunInfo(std::string_view id) {
    Session session(root_);
    const auto& pool = session.catalog.poolForLun(id);
    session.backend(pool);
    const auto entry = pool.volumes.find(id);
    return summary(pool, entry->first, entry->second);
}

LunInfo ProvisioningManager::fileLun(std::string_view id, std::string_view fileCatalog,
                                     std::optional<bool> writing) {
    Session session(root_);
    const auto& pool = session.catalog.poolForLun(id);
    const auto volume = pool.volumes.find(id);
    if (fileCatalog.empty() || volume->second.fileCatalog != fileCatalog) {
        throw std::invalid_argument("LUN is not file-managed by this storage catalog: " + std::string(id));
    }
    auto& array = session.backend(pool);
    if (writing.has_value()) {
        requireHealth(array, *writing);
    }
    return summary(pool, volume->first, volume->second);
}

std::vector<LunInfo> ProvisioningManager::listLuns() {
    Session session(root_);
    std::vector<LunInfo> result;
    for (const auto& entry : session.catalog.pools()) {
        for (const auto& lun : entry.second.volumes) {
            result.push_back(summary(entry.second, lun.first, lun.second));
        }
    }
    return result;
}

LunMapping ProvisioningManager::map(std::string_view id, std::uint64_t lba) {
    Session session(root_);
    const auto& pool = session.catalog.poolForLun(id);
    const auto& volume = pool.volumes.find(id)->second;
    checkRange(volume, lba, 1);
    session.backend(pool);
    return mappingAt(volume, lba, pool.options.chunkBlocks);
}

LunMapping ProvisioningManager::readBlocks(std::string_view id, std::uint64_t lba, std::uint64_t count,
                                           std::uint8_t* buffer, std::size_t bufferSize) {
    scheduling::ReadDemand demand(telemetry::foregroundRead());
    Session session(root_);
    const auto& pool = session.catalog.poolForLun(id);
    const auto& volume = pool.volumes.find(id)->second;
    checkTransfer(volume, lba, count, buffer, bufferSize);
    auto& array = session.backend(pool);
    requireHealth(array, false);
    chunks(lba, count, pool.options.chunkBlocks, [&](auto logical, auto offset, auto blocks, auto pos, auto bytes) {
        const auto mapping = volume.mappings.find(logical);
        if (mapping == volume.mappings.end()) {
            std::fill_n(buffer + pos, bytes, std::uint8_t{0});
        } else {
            array.readBlocks(mapping->second * pool.options.chunkBlocks + offset, blocks, buffer + pos, bytes);
        }
    });
    const auto mapped = mappingAt(volume, lba, pool.options.chunkBlocks);
    demand.success(count);
    return mapped;
}

LunWriteResult ProvisioningManager::writeBlocks(std::string_view id, std::uint64_t lba, std::uint64_t count,
                                                const std::uint8_t* buffer, std::size_t bufferSize) {
    return writeBlocksImpl(id, lba, count, buffer, bufferSize, {});
}

LunWriteResult ProvisioningManager::writeBlocksImpl(std::string_view id, std::uint64_t lba, std::uint64_t count,
                                                    const std::uint8_t* buffer, std::size_t bufferSize,
                                                    std::string_view fileCatalog) {
    Session session(root_);
    auto& pool = session.catalog.poolForLun(id);
    auto& volume = pool.volumes.find(id)->second;
    if (volume.fileCatalog != fileCatalog) {
        throw std::invalid_argument("Raw LUN write rejected. File-managed LUN ownership is required");
    }
    checkTransfer(volume, lba, count, buffer, bufferSize);
    auto& array = session.backend(pool);
    requireHealth(array, true);

    // Any reservation read while holding this lock belongs to an earlier, finished or interrupted writer.
    // Its chunks have no published mapping and are safe to reuse only via full initialization.
    const bool orphaned = !pool.reserved.empty();
    pool.reserved.clear();
    pool.reservedLun.clear();
    reserveChunks(pool, volume, id, lba, count);
    const auto newlyAllocated = static_cast<std::uint64_t>(pool.reserved.size());
    const bool allocating = !pool.reserved.empty();
    const auto chunkBlocks = pool.options.chunkBlocks;
    std::vector<std::uint8_t> initialized(allocating ? chunkBlocks * raidBlockSize : 0);
    if (orphaned || allocating) {
        observe(AllocationPhase::BeforeReservation);
        session.catalog.save(pool);
    }
    try {
        if (allocating) {
            observe(AllocationPhase::Reserved);
        }
        chunks(lba, count, chunkBlocks, [&](auto logical, auto offset, auto blocks, auto pos, auto bytes) {
            observe(AllocationPhase::BeforeChunkWrite);
            const auto existing = volume.mappings.find(logical);
            if (existing != volume.mappings.end()) {
                array.writePoolBlocks(existing->second * chunkBlocks + offset, blocks, buffer + pos, bytes);
            } else {
                // Write a full chunk with zeros everywhere outside the supplied data.
                std::fill(initialized.begin(), initialized.end(), std::uint8_t{0});
                std::copy_n(buffer + pos, bytes, initialized.data() + offset * raidBlockSize);
                const auto physical = pool.reserved.at(logical);
                array.writePoolBlocks(physical * chunkBlocks, chunkBlocks, initialized.data(), initialized.size());
            }
            observe(AllocationPhase::ChunkWritten);
        });
        if (allocating) {
            // Each RAID logical write flushes its members before returning. The replacement publishes
            // every new mapping and clears the reservation together.
            volume.mappings.merge(pool.reserved);
            pool.reservedLun.clear();
            observe(AllocationPhase::BeforeCommit);
            session.catalog.save(pool);
        }
    } catch (const std::exception& error) {
        throw std::runtime_error("LUN write interrupted or incomplete. Existing mapped blocks may have changed; "
                                 "uncommitted allocations remain unreadable. " + std::string(error.what()));
    }
    if (allocating) {
        observe(AllocationPhase::Committed);
    }
    return {newlyAllocated, volume.mappings.size(), chunkBlocks, volume.mappings.at(lba / chunkBlocks)};
}

}  // namespace raid
