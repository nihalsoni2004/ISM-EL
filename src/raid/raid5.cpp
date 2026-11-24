#include "raid/raid5.h"

#include "raid/parity.h"

#include <algorithm>
#include <stdexcept>

namespace raid {
namespace {

constexpr std::uint64_t parityBatchBlocks = 64;

std::size_t positionOnDisk(std::size_t dataIndex, std::size_t parityIndex) noexcept {
    return dataIndex < parityIndex ? dataIndex : dataIndex + 1;
}

std::size_t transferBytes(std::uint64_t blocks) noexcept {
    return static_cast<std::size_t>(blocks * raidBlockSize);
}

}  // namespace

Raid5::Raid5(RaidConfiguration configuration, Disks members, RaidWriteJournal journal,
             bool interruptedWrite, UnavailableMembers unavailable)
    : RaidController(std::move(configuration), std::move(members), RaidLevel::Raid5, std::move(unavailable)),
      journal_(std::move(journal)), inconsistent_(interruptedWrite) {
    if (this->configuration().layout != raid5Layout) {
        throw std::invalid_argument("Unsupported RAID 5 parity layout");
    }
    if (static_cast<bool>(journal_.begin) != static_cast<bool>(journal_.complete)) {
        throw std::invalid_argument("RAID 5 write marker requires both begin and complete operations");
    }
}

RaidStatus Raid5::status() const noexcept {
    return health(inconsistent_);
}

std::uint64_t Raid5::rawCapacityBytes() const noexcept {
    return capacityBlocks() / (memberCount() - 1) * memberCount() * raidBlockSize;
}

std::uint64_t Raid5::blocksPerRow() const noexcept {
    return (memberCount() - 1) * configuration().stripeUnitBlocks;
}

std::size_t Raid5::parityIndex(std::uint64_t row) const noexcept {
    return memberCount() - 1 - static_cast<std::size_t>(row % memberCount());
}

Raid5Mapping Raid5::locate(std::uint64_t lba) const noexcept {
    const auto unit = configuration().stripeUnitBlocks;
    const auto row = lba / blocksPerRow();
    const auto offset = lba % blocksPerRow();
    const auto parity = parityIndex(row);
    const auto data = positionOnDisk(static_cast<std::size_t>(offset / unit), parity);
    const auto physical = row * unit + offset % unit;
    return {lba, row, {data, physical}, {parity, physical}};
}

Raid5Mapping Raid5::inspectMapping(std::uint64_t lba) const {
    validateRange(lba, 1);
    return locate(lba);
}

std::vector<RaidLocation> Raid5::map(std::uint64_t lba) const {
    return {inspectMapping(lba).data};
}

void Raid5::requireConsistent() const {
    if (inconsistent_) {
        throw std::runtime_error("RAID 5 is inconsistent. Parity recovery is required before data access.");
    }
}

void Raid5::readPhysical(RaidLocation location, std::uint64_t count, std::uint8_t* buffer) {
    try {
        member(location.memberIndex).readBlocks(location.physicalLba, count, buffer, transferBytes(count));
    } catch (const std::exception& error) {
        markUnavailable(location.memberIndex);
        throw std::runtime_error("RAID 5 read failed on " + configuration().members[location.memberIndex] +
                                 ": " + error.what());
    }
    counts_.physicalBlockReads += count;
    ++counts_.readRequests;
}

void Raid5::writePhysical(RaidLocation location, std::uint64_t count, const std::uint8_t* buffer) {
    try {
        member(location.memberIndex).writeBlocks(location.physicalLba, count, buffer, transferBytes(count));
    } catch (const std::exception& error) {
        throw std::runtime_error("RAID 5 write failed on " + configuration().members[location.memberIndex] +
                                 ": " + error.what());
    }
    counts_.physicalBlockWrites += count;
    ++counts_.writeRequests;
}

void Raid5::reconstruct(RaidLocation missing, std::uint8_t* buffer) {
    ParityBlock recovered{};
    ParityBlock part{};
    for (std::size_t index = 0; index < memberCount(); ++index) {
        if (index != missing.memberIndex) {
            readPhysical({index, missing.physicalLba}, 1, part.data());
            xorInto(recovered.data(), recovered.size(), part.data(), part.size());
        }
    }
    std::copy(recovered.begin(), recovered.end(), buffer);
}

void Raid5::readBlocksImpl(std::uint64_t lba, std::uint64_t count, std::uint8_t* buffer,
                       std::size_t bufferSize) {
    counts_ = {};
    counts_.logicalBlocks = count;
    validateTransfer(lba, count, buffer, bufferSize);
    requireConsistent();
    if (status() == RaidStatus::Unavailable) {
        throw std::runtime_error("RAID 5 cannot read with two or more unavailable members");
    }

    const auto unit = configuration().stripeUnitBlocks;
    while (count != 0) {
        const auto mapping = locate(lba);
        const auto blocks = std::min(count, unit - lba % unit);
        bool loaded = false;
        if (available(mapping.data.memberIndex)) {
            try {
                readPhysical(mapping.data, blocks, buffer);
                loaded = true;
            } catch (const std::runtime_error&) {
                if (status() != RaidStatus::Degraded) {
                    throw;
                }
            }
        }
        if (!loaded) {
            for (std::uint64_t offset = 0; offset < blocks; ++offset) {
                reconstruct({mapping.data.memberIndex, mapping.data.physicalLba + offset},
                            buffer + transferBytes(offset));
            }
        }
        lba += blocks;
        count -= blocks;
        buffer += transferBytes(blocks);
    }
}

void Raid5::updateBlock(const Raid5Mapping& mapping, const std::uint8_t* buffer, std::vector<bool>& touched) {
    ParityBlock oldData{};
    ParityBlock parity{};
    readPhysical(mapping.data, 1, oldData.data());
    readPhysical(mapping.parity, 1, parity.data());
    xorInto(parity.data(), parity.size(), oldData.data(), oldData.size());
    xorInto(parity.data(), parity.size(), buffer, raidBlockSize);
    writePhysical(mapping.data, 1, buffer);
    writePhysical(mapping.parity, 1, parity.data());
    touched[mapping.data.memberIndex] = true;
    touched[mapping.parity.memberIndex] = true;
}

void Raid5::writeFullRow(std::uint64_t row, const std::uint8_t* buffer, std::vector<bool>& touched) {
    const auto unit = configuration().stripeUnitBlocks;
    const auto parityDisk = parityIndex(row);
    const auto dataDisks = memberCount() - 1;
    std::vector<std::uint8_t> parity(transferBytes(std::min(unit, parityBatchBlocks)));

    // Complete corresponding windows of each unit, keeping parity memory bounded.
    for (std::uint64_t offset = 0; offset < unit;) {
        const auto blocks = std::min(parityBatchBlocks, unit - offset);
        const auto bytes = transferBytes(blocks);
        const auto physicalLba = row * unit + offset;
        std::fill_n(parity.data(), bytes, std::uint8_t{0});
        for (std::size_t index = 0; index < dataDisks; ++index) {
            const auto* data = buffer + transferBytes(index * unit + offset);
            xorInto(parity.data(), bytes, data, bytes);
        }
        for (std::size_t index = 0; index < dataDisks; ++index) {
            const auto* data = buffer + transferBytes(index * unit + offset);
            const auto disk = positionOnDisk(index, parityDisk);
            writePhysical({disk, physicalLba}, blocks, data);
        }
        writePhysical({parityDisk, physicalLba}, blocks, parity.data());
        offset += blocks;
    }
    std::fill(touched.begin(), touched.end(), true);
}

void Raid5::writeBlocksImpl(std::uint64_t lba, std::uint64_t count, const std::uint8_t* buffer,
                        std::size_t bufferSize) {
    counts_ = {};
    counts_.logicalBlocks = count;
    validateTransfer(lba, count, buffer, bufferSize);
    requireConsistent();
    if (status() != RaidStatus::Healthy) {
        throw std::runtime_error("RAID 5 requires every member for writes");
    }

    std::vector<bool> touched(memberCount(), false);
    if (journal_.begin) {
        journal_.begin();
    }
    inconsistent_ = true;
    try {
        const auto width = blocksPerRow();
        while (count != 0) {
            const auto segment = std::min(count, width - lba % width);
            if (segment == width) {
                ++counts_.fullStripeRows;
                writeFullRow(lba / width, buffer, touched);
            } else {
                ++counts_.partialStripeRows;
                for (std::uint64_t offset = 0; offset < segment; ++offset) {
                    updateBlock(locate(lba + offset), buffer + transferBytes(offset), touched);
                }
            }
            lba += segment;
            count -= segment;
            buffer += transferBytes(segment);
        }

        for (std::size_t index = 0; index < memberCount(); ++index) {
            if (touched[index]) {
                try {
                    member(index).flush();
                } catch (const std::exception& error) {
                    throw std::runtime_error("RAID 5 flush failed on " + configuration().members[index] +
                                             ": " + error.what());
                }
            }
        }
        if (journal_.complete) {
            journal_.complete();
        }
    } catch (const std::exception& error) {
        throw std::runtime_error("Incomplete RAID 5 write. Array is inconsistent: " + std::string(error.what()));
    }
    inconsistent_ = false;
}

Raid5Verification Raid5::verifyParity() {
    const telemetry::Scope scope(telemetry::Origin::Internal, configuration().id);
    requireConsistent();
    if (status() != RaidStatus::Healthy) {
        throw std::runtime_error("RAID 5 parity verification requires every member");
    }
    counts_ = {};
    counts_.logicalBlocks = capacityBlocks();
    const auto unit = configuration().stripeUnitBlocks;
    const auto rows = capacityBlocks() / blocksPerRow();
    std::vector<std::uint8_t> parity(transferBytes(std::min(unit, parityBatchBlocks)));
    auto part = parity;
    Raid5Verification result;
    for (std::uint64_t row = 0; row < rows; ++row) {
        const auto parityDisk = parityIndex(row);
        bool rowMismatch = false;
        for (std::uint64_t offset = 0; offset < unit;) {
            const auto blocks = std::min(parityBatchBlocks, unit - offset);
            const auto bytes = transferBytes(blocks);
            const auto lba = row * unit + offset;
            std::fill_n(parity.data(), bytes, std::uint8_t{0});
            for (std::size_t disk = 0; disk < memberCount(); ++disk) {
                if (disk != parityDisk) {
                    readPhysical({disk, lba}, blocks, part.data());
                    xorInto(parity.data(), bytes, part.data(), bytes);
                }
            }
            readPhysical({parityDisk, lba}, blocks, part.data());
            if (!rowMismatch && !std::equal(parity.data(), parity.data() + bytes, part.data())) {
                rowMismatch = true;
                ++result.parityMismatches;
                if (!inconsistent_) {
                    inconsistent_ = true;
                    if (journal_.begin) {
                        journal_.begin();
                    }
                }
            }
            offset += blocks;
        }
        ++result.rowsChecked;
    }
    return result;
}

void Raid5::verify() {
    const auto report = verifyParity();
    if (report.parityMismatches != 0) {
        throw std::runtime_error("RAID 5 parity mismatch in " + std::to_string(report.parityMismatches) +
                                 " stripe rows. Parity recovery is required.");
    }
}

void Raid5::reconstructMemberBlock(std::size_t slot, std::uint64_t physicalLba,
                                  std::uint8_t* buffer, std::size_t bufferSize, bool) {
    validateRebuildBlock(slot, physicalLba, buffer, bufferSize);
    reconstruct({slot, physicalLba}, buffer);
}

}  // namespace raid
