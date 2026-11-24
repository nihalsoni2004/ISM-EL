#include "raid/raid0.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace raid {

Raid0::Raid0(RaidConfiguration configuration, Disks members, UnavailableMembers unavailable,
             bool interruptedWrite)
    : RaidController(std::move(configuration), std::move(members), RaidLevel::Raid0, std::move(unavailable)),
      inconsistent_(interruptedWrite) {}

RaidStatus Raid0::status() const noexcept {
    return health(inconsistent_);
}

RaidLocation Raid0::locate(std::uint64_t lba) const noexcept {
    const auto unit = configuration().stripeUnitBlocks;
    const auto stripe = lba / unit;
    return {static_cast<std::size_t>(stripe % memberCount()),
            (stripe / memberCount()) * unit + lba % unit};
}

std::vector<RaidLocation> Raid0::map(std::uint64_t lba) const {
    validateRange(lba, 1);
    return {locate(lba)};
}

void Raid0::transfer(std::uint64_t lba, std::uint64_t count,
                     const std::function<void(VirtualDisk&, std::uint64_t, std::uint64_t, std::size_t)>& io) {
    if (status() != RaidStatus::Healthy) {
        if (inconsistent_) {
            throw std::runtime_error("RAID 0 is inconsistent. Recovery is required before data access.");
        }
        throw std::runtime_error("RAID 0 requires all members to be available");
    }
    const auto unit = configuration().stripeUnitBlocks;
    std::size_t offset = 0;
    while (count != 0) {
        const auto location = locate(lba);
        const auto blocks = std::min(count, unit - lba % unit);
        try {
            io(member(location.memberIndex), location.physicalLba, blocks, offset);
        } catch (const std::exception& error) {
            markUnavailable(location.memberIndex);
            throw std::runtime_error("RAID 0 member " + configuration().members[location.memberIndex] +
                                     " I/O failed: " + error.what());
        }
        lba += blocks;
        count -= blocks;
        offset += static_cast<std::size_t>(blocks * raidBlockSize);
    }
}

void Raid0::readBlocksImpl(std::uint64_t lba, std::uint64_t count, std::uint8_t* buffer,
                       std::size_t bufferSize) {
    validateTransfer(lba, count, buffer, bufferSize);
    transfer(lba, count, [buffer](VirtualDisk& disk, auto physical, auto blocks, auto offset) {
        disk.readBlocks(physical, blocks, buffer + offset, static_cast<std::size_t>(blocks * raidBlockSize));
    });
}

void Raid0::writeBlocksImpl(std::uint64_t lba, std::uint64_t count, const std::uint8_t* buffer,
                        std::size_t bufferSize) {
    validateTransfer(lba, count, buffer, bufferSize);
    transfer(lba, count, [buffer](VirtualDisk& disk, auto physical, auto blocks, auto offset) {
        disk.writeBlocks(physical, blocks, buffer + offset, static_cast<std::size_t>(blocks * raidBlockSize));
        disk.flush();
    });
}

void Raid0::verify() {
    const telemetry::Scope scope(telemetry::Origin::Internal, configuration().id);
    std::array<std::uint8_t, raidBlockSize> block{};
    for (std::size_t index = 0; index < memberCount(); ++index) {
        readBlocksImpl(index * configuration().stripeUnitBlocks, 1, block.data(), block.size());
    }
    readBlocksImpl(capacityBlocks() - 1, 1, block.data(), block.size());
}

}  // namespace raid
