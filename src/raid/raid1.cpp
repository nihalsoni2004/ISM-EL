#include "raid/raid1.h"

#include <algorithm>
#include <stdexcept>

namespace raid {

Raid1::Raid1(RaidConfiguration configuration, Disks members, MirrorWriteJournal journal,
             bool interruptedWrite, UnavailableMembers unavailable)
    : RaidController(std::move(configuration), std::move(members), RaidLevel::Raid1, std::move(unavailable)),
      journal_(std::move(journal)), inconsistent_(interruptedWrite) {
    if (static_cast<bool>(journal_.begin) != static_cast<bool>(journal_.complete)) {
        throw std::invalid_argument("Mirror journal requires both begin and complete operations");
    }
}

RaidStatus Raid1::status() const noexcept {
    return health(inconsistent_);
}

std::vector<RaidLocation> Raid1::map(std::uint64_t lba) const {
    validateRange(lba, 1);
    return {{0, lba}, {1, lba}};
}

void Raid1::requireConsistent() const {
    if (inconsistent_) {
        throw std::runtime_error("RAID 1 is inconsistent. Mirror recovery is required before data access.");
    }
}

void Raid1::readBlocksImpl(std::uint64_t lba, std::uint64_t count, std::uint8_t* buffer,
                       std::size_t bufferSize) {
    const auto bytes = validateTransfer(lba, count, buffer, bufferSize);
    requireConsistent();
    std::string lastFailure;
    for (std::size_t index = 0; index < memberCount(); ++index) {
        if (available(index)) {
            try {
                member(index).readBlocks(lba, count, buffer, bytes);
                return;
            } catch (const std::exception& error) {
                markUnavailable(index);
                lastFailure = ": " + configuration().members[index] + ": " + error.what();
            }
        }
    }
    throw std::runtime_error("RAID 1 has no readable member" + lastFailure);
}

void Raid1::writeBlocksImpl(std::uint64_t lba, std::uint64_t count, const std::uint8_t* buffer,
                        std::size_t bufferSize) {
    const auto bytes = validateTransfer(lba, count, buffer, bufferSize);
    requireConsistent();
    if (status() != RaidStatus::Healthy) {
        throw std::runtime_error("RAID 1 requires both members for a mirrored write");
    }

    if (journal_.begin) {
        journal_.begin();
    }
    inconsistent_ = true;
    for (std::size_t index = 0; index < memberCount(); ++index) {
        try {
            member(index).writeBlocks(lba, count, buffer, bytes);
            member(index).flush();
        } catch (const std::exception& error) {
            throw std::runtime_error("Incomplete RAID 1 write on " + configuration().members[index] +
                                     "; the array is inconsistent: " + error.what());
        }
    }
    if (journal_.complete) {
        journal_.complete();
    }
    inconsistent_ = false;
}

void Raid1::verify() {
    const telemetry::Scope scope(telemetry::Origin::Internal, configuration().id);
    requireConsistent();
    if (status() != RaidStatus::Healthy) {
        throw std::runtime_error("RAID 1 verification requires both members");
    }
    constexpr std::uint64_t chunkBlocks = 64;
    std::vector<std::uint8_t> left(chunkBlocks * raidBlockSize);
    auto right = left;
    for (std::uint64_t lba = 0; lba < capacityBlocks();) {
        const auto blocks = std::min(chunkBlocks, capacityBlocks() - lba);
        const auto bytes = static_cast<std::size_t>(blocks * raidBlockSize);
        const auto read = [&](std::size_t index, std::vector<std::uint8_t>& buffer) {
            try {
                member(index).readBlocks(lba, blocks, buffer.data(), bytes);
            } catch (const std::exception& error) {
                markUnavailable(index);
                throw std::runtime_error("Cannot verify RAID 1 member " + configuration().members[index] +
                                         ": " + error.what());
            }
        };
        read(0, left);
        read(1, right);
        const auto difference = std::mismatch(left.data(), left.data() + bytes, right.data());
        if (difference.first != left.data() + bytes) {
            inconsistent_ = true;
            if (journal_.begin) {
                journal_.begin();
            }
            const auto mismatchedLba = lba + static_cast<std::uint64_t>(difference.first - left.data()) / raidBlockSize;
            throw std::runtime_error("RAID 1 mirrors differ at logical LBA " + std::to_string(mismatchedLba) +
                                     "; mirror recovery is required");
        }
        lba += blocks;
    }
}

void Raid1::reconstructMemberBlock(std::size_t slot, std::uint64_t physicalLba,
                                  std::uint8_t* buffer, std::size_t bufferSize, bool) {
    validateRebuildBlock(slot, physicalLba, buffer, bufferSize);
    member(1 - slot).readBlocks(physicalLba, 1, buffer, bufferSize);
}

}  // namespace raid
