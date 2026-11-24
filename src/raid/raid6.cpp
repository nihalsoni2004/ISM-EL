#include "raid/raid6.h"

#include "raid/dual_parity.h"

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace raid {
namespace {

constexpr std::uint64_t parityBatchBlocks = 64;

std::size_t transferBytes(std::uint64_t blocks) noexcept {
    return static_cast<std::size_t>(blocks * raidBlockSize);
}

}  // namespace

Raid6::Raid6(RaidConfiguration configuration, Disks members, RaidWriteJournal journal,
             bool interruptedWrite, MemberFlush flushMember, UnavailableMembers unavailable)
    : RaidController(std::move(configuration), std::move(members), RaidLevel::Raid6, std::move(unavailable)),
      journal_(std::move(journal)), flushMember_(std::move(flushMember)), inconsistent_(interruptedWrite) {
    if (this->configuration().layout != raid6Layout) {
        throw std::invalid_argument("Unsupported RAID 6 parity layout");
    }
    if (static_cast<bool>(journal_.begin) != static_cast<bool>(journal_.complete) || !flushMember_) {
        throw std::invalid_argument("RAID 6 requires a flush operation and a complete write-marker pair");
    }
}

RaidStatus Raid6::status() const noexcept {
    return health(inconsistent_);
}

std::uint64_t Raid6::rawCapacityBytes() const noexcept {
    return capacityBlocks() / (memberCount() - 2) * memberCount() * raidBlockSize;
}

std::uint64_t Raid6::blocksPerRow() const noexcept {
    return (memberCount() - 2) * configuration().stripeUnitBlocks;
}

std::size_t Raid6::ParityMembers::dataMember(std::size_t index) const noexcept {
    if (index >= std::min(p, q)) {
        ++index;
    }
    if (index >= std::max(p, q)) {
        ++index;
    }
    return index;
}

Raid6::ParityMembers Raid6::parityMembers(std::uint64_t row) const noexcept {
    const auto p = memberCount() - 1 - static_cast<std::size_t>(row % memberCount());
    return {p, (p + memberCount() - 1) % memberCount()};
}

Raid6Mapping Raid6::locate(std::uint64_t lba) const noexcept {
    const auto unit = configuration().stripeUnitBlocks;
    const auto width = blocksPerRow();
    const auto row = lba / width;
    const auto index = static_cast<std::size_t>(lba % width / unit);
    const auto physical = row * unit + lba % unit;
    const auto parity = parityMembers(row);
    return {configuration().id, lba, row, index, {parity.dataMember(index), physical},
            {parity.p, physical}, {parity.q, physical}};
}

Raid6Mapping Raid6::inspectMapping(std::uint64_t lba) const {
    validateRange(lba, 1);
    return locate(lba);
}

std::vector<RaidLocation> Raid6::map(std::uint64_t lba) const {
    return {inspectMapping(lba).data};
}

void Raid6::requireConsistent() const {
    if (inconsistent_) {
        throw std::runtime_error("RAID 6 is inconsistent. Parity recovery is required before data access.");
    }
}

void Raid6::readPhysical(RaidLocation location, std::uint64_t count, std::uint8_t* buffer) {
    try {
        member(location.memberIndex).readBlocks(location.physicalLba, count, buffer, transferBytes(count));
    } catch (const std::exception& error) {
        markUnavailable(location.memberIndex);
        throw std::runtime_error("RAID 6 read failed on " + configuration().members[location.memberIndex] +
                                 ": " + error.what());
    }
    counts_.physicalBlockReads += count;
    ++counts_.readRequests;
}

void Raid6::writePhysical(RaidLocation location, std::uint64_t count, const std::uint8_t* buffer) {
    try {
        member(location.memberIndex).writeBlocks(location.physicalLba, count, buffer, transferBytes(count));
    } catch (const std::exception& error) {
        throw std::runtime_error("RAID 6 write failed on " + configuration().members[location.memberIndex] +
                                 ": " + error.what());
    }
    counts_.physicalBlockWrites += count;
    ++counts_.writeRequests;
}

void Raid6::reconstruct(const Raid6Mapping& mapping, std::uint64_t offset, std::uint8_t* buffer) {
    const auto parity = parityMembers(mapping.stripeRow);
    const auto physical = mapping.data.physicalLba + offset;
    const auto units = memberCount() - 2;
    std::optional<std::size_t> secondMissing;
    for (std::size_t index = 0; index < units; ++index) {
        if (index != mapping.dataUnitIndex && !available(parity.dataMember(index))) {
            secondMissing = index;
            break;
        }
    }

    const bool useP = available(parity.p);
    const bool useQ = !useP || secondMissing.has_value();
    ParityBlock p{}, q{}, survivor{};
    if (useP) {
        readPhysical({parity.p, physical}, 1, p.data());
    }
    if (useQ) {
        readPhysical({parity.q, physical}, 1, q.data());
    }
    for (std::size_t index = 0; index < units; ++index) {
        if (index == mapping.dataUnitIndex || index == secondMissing) {
            continue;
        }
        readPhysical({parity.dataMember(index), physical}, 1, survivor.data());
        if (useP) {
            xorInto(p.data(), p.size(), survivor.data(), survivor.size());
        }
        if (useQ) {
            dual_parity::xorScaled(q.data(), survivor.data(), q.size(), dual_parity::coefficient(index));
        }
    }
    if (secondMissing) {
        const auto recovered = dual_parity::recoverTwo(p, q, dual_parity::coefficient(mapping.dataUnitIndex),
                                                       dual_parity::coefficient(*secondMissing));
        std::copy(recovered.first.begin(), recovered.first.end(), buffer);
    } else if (!useP) {
        const auto recovered = dual_parity::recoverFromQ(q, dual_parity::coefficient(mapping.dataUnitIndex));
        std::copy(recovered.begin(), recovered.end(), buffer);
    } else {
        std::copy(p.begin(), p.end(), buffer);
    }
}

void Raid6::readBlocksImpl(std::uint64_t lba, std::uint64_t count, std::uint8_t* buffer,
                       std::size_t bufferSize) {
    counts_ = {};
    counts_.logicalBlocks = count;
    validateTransfer(lba, count, buffer, bufferSize);
    requireConsistent();
    if (status() == RaidStatus::Unavailable) {
        throw std::runtime_error("RAID 6 cannot read with more than two unavailable members");
    }

    const auto unit = configuration().stripeUnitBlocks;
    while (count != 0) {
        const auto mapping = locate(lba);
        const auto blocks = std::min(count, unit - lba % unit);
        if (available(mapping.data.memberIndex)) {
            readPhysical(mapping.data, blocks, buffer);
        } else {
            for (std::uint64_t offset = 0; offset < blocks; ++offset) {
                reconstruct(mapping, offset, buffer + transferBytes(offset));
            }
        }
        lba += blocks;
        count -= blocks;
        buffer += transferBytes(blocks);
    }
}

void Raid6::updateBlock(const Raid6Mapping& mapping, const std::uint8_t* buffer, std::vector<bool>& touched) {
    ParityBlock delta{}, p{}, q{};
    readPhysical(mapping.data, 1, delta.data());
    readPhysical(mapping.p, 1, p.data());
    readPhysical(mapping.q, 1, q.data());
    xorInto(delta.data(), delta.size(), buffer, raidBlockSize);
    dual_parity::accumulate(p.data(), q.data(), delta.data(), delta.size(),
                            dual_parity::coefficient(mapping.dataUnitIndex));
    writePhysical(mapping.data, 1, buffer);
    writePhysical(mapping.p, 1, p.data());
    writePhysical(mapping.q, 1, q.data());
    touched[mapping.data.memberIndex] = true;
    touched[mapping.p.memberIndex] = true;
    touched[mapping.q.memberIndex] = true;
}

void Raid6::writeFullRow(std::uint64_t row, const std::uint8_t* buffer, std::vector<bool>& touched) {
    const auto unit = configuration().stripeUnitBlocks;
    const auto parity = parityMembers(row);
    const auto units = memberCount() - 2;
    std::vector<std::uint8_t> p(transferBytes(std::min(unit, parityBatchBlocks)));
    auto q = p;

    for (std::uint64_t offset = 0; offset < unit;) {
        const auto count = std::min(parityBatchBlocks, unit - offset);
        const auto bytes = transferBytes(count);
        const auto physical = row * unit + offset;
        std::fill_n(p.data(), bytes, std::uint8_t{0});
        std::fill_n(q.data(), bytes, std::uint8_t{0});
        for (std::size_t index = 0; index < units; ++index) {
            const auto* data = buffer + transferBytes(index * unit + offset);
            dual_parity::accumulate(p.data(), q.data(), data, bytes, dual_parity::coefficient(index));
        }
        for (std::size_t index = 0; index < units; ++index) {
            writePhysical({parity.dataMember(index), physical}, count, buffer + transferBytes(index * unit + offset));
        }
        writePhysical({parity.p, physical}, count, p.data());
        writePhysical({parity.q, physical}, count, q.data());
        offset += count;
    }
    std::fill(touched.begin(), touched.end(), true);
}

void Raid6::writeBlocksImpl(std::uint64_t lba, std::uint64_t count, const std::uint8_t* buffer,
                        std::size_t bufferSize) {
    counts_ = {};
    counts_.logicalBlocks = count;
    validateTransfer(lba, count, buffer, bufferSize);
    requireConsistent();
    if (status() != RaidStatus::Healthy) {
        throw std::runtime_error("RAID 6 requires every member for writes");
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
                    flushMember_(member(index));
                } catch (const std::exception& error) {
                    throw std::runtime_error("RAID 6 flush failed on " + configuration().members[index] +
                                             ": " + error.what());
                }
            }
        }
        if (journal_.complete) {
            journal_.complete();
        }
    } catch (const std::exception& error) {
        throw std::runtime_error("Incomplete RAID 6 write. Array is inconsistent: " + std::string(error.what()));
    }
    inconsistent_ = false;
}

Raid6Verification Raid6::verifyParity() {
    const telemetry::Scope scope(telemetry::Origin::Internal, configuration().id);
    requireConsistent();
    if (status() != RaidStatus::Healthy) {
        throw std::runtime_error("RAID 6 verification requires every member");
    }
    counts_ = {};
    counts_.logicalBlocks = capacityBlocks();
    const auto unit = configuration().stripeUnitBlocks;
    const auto rows = capacityBlocks() / blocksPerRow();
    const auto units = memberCount() - 2;
    std::vector<std::uint8_t> p(transferBytes(std::min(unit, parityBatchBlocks)));
    auto q = p;
    auto stored = p;
    Raid6Verification result;
    const auto recordMismatch = [this] {
        if (!inconsistent_) {
            inconsistent_ = true;
            if (journal_.begin) {
                journal_.begin();
            }
        }
    };
    for (std::uint64_t row = 0; row < rows; ++row) {
        const auto parity = parityMembers(row);
        bool badP = false;
        bool badQ = false;
        for (std::uint64_t offset = 0; offset < unit;) {
            const auto count = std::min(parityBatchBlocks, unit - offset);
            const auto bytes = transferBytes(count);
            const auto lba = row * unit + offset;
            std::fill_n(p.data(), bytes, std::uint8_t{0});
            std::fill_n(q.data(), bytes, std::uint8_t{0});
            for (std::size_t index = 0; index < units; ++index) {
                readPhysical({parity.dataMember(index), lba}, count, stored.data());
                dual_parity::accumulate(p.data(), q.data(), stored.data(), bytes, dual_parity::coefficient(index));
            }
            readPhysical({parity.p, lba}, count, stored.data());
            badP |= !std::equal(p.data(), p.data() + bytes, stored.data());
            if (badP) {
                recordMismatch();
            }
            readPhysical({parity.q, lba}, count, stored.data());
            badQ |= !std::equal(q.data(), q.data() + bytes, stored.data());
            if (badQ) {
                recordMismatch();
            }
            offset += count;
        }
        result.pMismatches += badP;
        result.qMismatches += badQ;
        result.rowsWithErrors += badP || badQ;
        ++result.rowsChecked;
    }
    return result;
}

void Raid6::verify() {
    const auto report = verifyParity();
    if (report.rowsWithErrors != 0) {
        throw std::runtime_error("RAID 6 parity mismatch in " + std::to_string(report.rowsWithErrors) +
                                 " stripe rows. Parity recovery is required.");
    }
}

void Raid6::reconstructMemberBlock(std::size_t slot, std::uint64_t physicalLba,
                                  std::uint8_t* buffer, std::size_t bufferSize, bool verifySurvivors) {
    validateRebuildBlock(slot, physicalLba, buffer, bufferSize);
    const auto unit = configuration().stripeUnitBlocks;
    const auto row = physicalLba / unit;
    const auto parity = parityMembers(row);
    const auto dataUnits = memberCount() - 2;
    const auto rebuildData = [&](std::size_t index, std::uint8_t* output) {
        reconstruct({configuration().id, 0, row, index, {parity.dataMember(index), physicalLba},
                     {parity.p, physicalLba}, {parity.q, physicalLba}}, 0, output);
    };

    const bool dataTarget = slot != parity.p && slot != parity.q;
    if (dataTarget) {
        std::size_t index = 0;
        while (parity.dataMember(index) != slot) {
            ++index;
        }
        rebuildData(index, buffer);
        if (!verifySurvivors) {
            return;
        }
    }

    ParityBlock p{}, q{}, data{};
    for (std::size_t index = 0; index < dataUnits; ++index) {
        const auto disk = parity.dataMember(index);
        if (disk == slot) {
            std::copy_n(buffer, data.size(), data.data());
        } else if (available(disk)) {
            readPhysical({disk, physicalLba}, 1, data.data());
        } else {
            rebuildData(index, data.data());
        }
        dual_parity::accumulate(p.data(), q.data(), data.data(), data.size(), dual_parity::coefficient(index));
    }
    if (verifySurvivors) {
        for (const auto& [disk, expected] : {std::make_pair(parity.p, &p), std::make_pair(parity.q, &q)}) {
            if (available(disk)) {
                readPhysical({disk, physicalLba}, 1, data.data());
                if (data != *expected) {
                    throw std::runtime_error("Surviving RAID 6 parity disagrees at physical LBA " +
                                             std::to_string(physicalLba));
                }
            }
        }
    }
    if (!dataTarget) {
        const auto& expected = slot == parity.p ? p : q;
        std::copy(expected.begin(), expected.end(), buffer);
    }
}

}  // namespace raid
