#include "raid/raid_controller.h"
#include "recovery/scheduling/scheduler.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace raid {

std::size_t failureTolerance(RaidLevel level) noexcept {
    switch (level) {
    case RaidLevel::Raid1:
    case RaidLevel::Raid5: return 1;
    case RaidLevel::Raid6: return 2;
    default: return 0;
    }
}

RaidHealth calculateRaidHealth(RaidLevel level, std::size_t unavailable, bool inconsistent) noexcept {
    if (inconsistent) {
        return RaidHealth::Inconsistent;
    }
    if (unavailable > failureTolerance(level)) {
        return RaidHealth::Failed;
    }
    return unavailable == 0 ? RaidHealth::Healthy : RaidHealth::Degraded;
}

void validateRaidLayout(RaidLevel level, std::size_t memberCount, std::uint64_t stripeUnitBlocks) {
    switch (level) {
    case RaidLevel::Raid0:
        if (memberCount < 2) {
            throw std::invalid_argument("RAID 0 requires at least two members");
        }
        if (stripeUnitBlocks == 0) {
            throw std::invalid_argument("RAID 0 stripe unit must be positive");
        }
        return;
    case RaidLevel::Raid1:
        if (memberCount != 2 || stripeUnitBlocks != mirrorStripeUnitBlocks) {
            throw std::invalid_argument("RAID 1 requires two members and a stripe unit of 1");
        }
        return;
    case RaidLevel::Raid5:
        if (memberCount < 3) {
            throw std::invalid_argument("RAID 5 requires at least three members");
        }
        if (stripeUnitBlocks == 0) {
            throw std::invalid_argument("RAID 5 stripe unit must be positive");
        }
        return;
    case RaidLevel::Raid6:
        if (memberCount < 4 || memberCount > maxRaid6Members) {
            throw std::invalid_argument("RAID 6 requires between 4 and 257 members");
        }
        if (stripeUnitBlocks == 0) {
            throw std::invalid_argument("RAID 6 stripe unit must be positive");
        }
        return;
    }
    throw std::invalid_argument("Unsupported RAID level");
}

std::uint64_t calculateRaidCapacity(RaidLevel level, std::size_t memberCount,
                                    std::uint64_t memberBlocks, std::uint64_t stripeUnitBlocks) {
    validateRaidLayout(level, memberCount, stripeUnitBlocks);
    constexpr auto maxBlocks = std::numeric_limits<std::uint64_t>::max() / raidBlockSize;
    if (memberBlocks == 0 || memberBlocks > maxBlocks) {
        throw std::invalid_argument("Invalid RAID member capacity");
    }
    if (level == RaidLevel::Raid1) {
        return memberBlocks;
    }
    if (memberBlocks % stripeUnitBlocks != 0) {
        throw std::invalid_argument("RAID stripe unit must divide the member block count");
    }
    if (memberCount > maxBlocks / memberBlocks) {
        throw std::overflow_error("RAID raw capacity exceeds the supported byte range");
    }
    return memberBlocks * dataUnitCount(level, memberCount);
}

RaidController::RaidController(RaidConfiguration configuration, Disks members, RaidLevel expectedLevel,
                               UnavailableMembers unavailable)
    : configuration_(std::move(configuration)), members_(std::move(members)),
      unavailable_(std::move(unavailable)) {
    if (unavailable_.empty()) {
        unavailable_.resize(members_.size(), false);
    }
    if (level() != expectedLevel || configuration_.blockSize != raidBlockSize ||
        configuration_.schemaVersion != raidSchemaVersion ||
        members_.size() != configuration_.members.size() || unavailable_.size() != members_.size()) {
        throw std::invalid_argument("Invalid RAID configuration");
    }
    validateRaidLayout(level(), memberCount(), configuration_.stripeUnitBlocks);
    const auto blocks = capacityBlocks() / dataUnitCount(level(), memberCount());
    if (capacityBlocks() != calculateRaidCapacity(level(), memberCount(), blocks, configuration_.stripeUnitBlocks)) {
        throw std::invalid_argument("Stored RAID capacity differs from member geometry");
    }
    std::set<const VirtualDisk*> handles;
    std::set<std::string_view> ids;
    for (std::size_t index = 0; index < memberCount(); ++index) {
        if (!ids.insert(configuration_.members[index]).second) {
            throw std::invalid_argument("Duplicate RAID member");
        }
        if (!unavailable_[index] &&
            (!available(index) || member(index).getBlockCount() != blocks || member(index).getBlockSize() != raidBlockSize)) {
            throw std::invalid_argument("Available RAID members must be open and match stored geometry");
        }
        if (members_[index] && !handles.insert(members_[index].get()).second) {
            throw std::invalid_argument("Duplicate RAID member handle");
        }
    }
}

void RaidController::readBlocks(std::uint64_t lba, std::uint64_t count,
                                std::uint8_t* buffer, std::size_t bufferSize) {
    const telemetry::Scope scope(configuration_.id);
    scheduling::ReadDemand demand(telemetry::origin() == telemetry::Origin::Foreground);
    telemetry::OperationTimer timer(telemetry::Kind::RaidRead, count, raidBlockSize);
    readBlocksImpl(lba, count, buffer, bufferSize);
    timer.success();
    demand.success(count);
}

void RaidController::writeBlocks(std::uint64_t lba, std::uint64_t count,
                                 const std::uint8_t* buffer, std::size_t bufferSize) {
    const telemetry::Scope scope(configuration_.id);
    telemetry::OperationTimer timer(telemetry::Kind::RaidWrite, count, raidBlockSize);
    if (poolOwned()) {
        throw std::invalid_argument("Raw RAID write rejected. " + configuration_.id + " is owned by a storage pool");
    }
    writeBlocksImpl(lba, count, buffer, bufferSize);
    timer.success();
}

void RaidController::writePoolBlocks(std::uint64_t lba, std::uint64_t count,
                                     const std::uint8_t* buffer, std::size_t bufferSize) {
    const telemetry::Scope scope(configuration_.id);
    telemetry::OperationTimer timer(telemetry::Kind::RaidWrite, count, raidBlockSize);
    if (!poolOwned()) {
        throw std::logic_error("Provisioning write requires pool ownership");
    }
    writeBlocksImpl(lba, count, buffer, bufferSize);
    timer.success();
}

void RaidController::markUnavailable(std::string_view id) {
    const auto& ids = configuration_.members;
    const auto found = std::find(ids.begin(), ids.end(), id);
    if (found == ids.end()) {
        throw std::invalid_argument("Disk is not a member of " + configuration_.id);
    }
    markUnavailable(static_cast<std::size_t>(found - ids.begin()));
}

bool RaidController::available(std::size_t index) const noexcept {
    return !unavailable_[index] && members_[index] && member(index).isOpen();
}

bool RaidController::memberAvailable(std::size_t index) const noexcept {
    return index < memberCount() && available(index);
}

RaidHealth RaidController::health(bool inconsistent) const noexcept {
    return calculateRaidHealth(level(), memberCount() - availableCount(), inconsistent);
}

void RaidController::reconstructMemberBlock(std::size_t, std::uint64_t, std::uint8_t*, std::size_t, bool) {
    throw std::runtime_error("This RAID level cannot reconstruct an unavailable member");
}

void RaidController::validateRebuildBlock(std::size_t slot, std::uint64_t physicalLba,
                                         const std::uint8_t* buffer, std::size_t bufferSize) const {
    if (status() != RaidHealth::Degraded || slot >= memberCount() || available(slot)) {
        throw std::runtime_error("Reconstruction requires a degraded array and an unavailable member slot");
    }
    if (!buffer || bufferSize < raidBlockSize ||
        physicalLba >= capacityBlocks() / dataUnitCount(level(), memberCount())) {
        throw std::invalid_argument("Invalid physical rebuild block or buffer");
    }
}

std::size_t RaidController::availableCount() const noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < memberCount(); ++index) {
        if (available(index)) {
            ++count;
        }
    }
    return count;
}

void RaidController::validateRange(std::uint64_t lba, std::uint64_t count) const {
    if (count == 0) {
        throw std::invalid_argument("RAID block count must be positive");
    }
    if (lba >= capacityBlocks() || count > capacityBlocks() - lba) {
        throw std::out_of_range("Logical block range is outside the RAID array");
    }
}

std::size_t RaidController::validateTransfer(std::uint64_t lba, std::uint64_t count,
                                             const void* buffer, std::size_t bufferSize) const {
    validateRange(lba, count);
    if (buffer == nullptr || count > bufferSize / raidBlockSize) {
        throw std::invalid_argument("RAID transfer requires a buffer large enough for the request");
    }
    return static_cast<std::size_t>(count * raidBlockSize);
}

}  // namespace raid
