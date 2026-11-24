#pragma once

#include "raid/raid_controller.h"

namespace raid {

struct Raid5Mapping {
    std::uint64_t logicalLba;
    std::uint64_t stripeRow;
    RaidLocation data;
    RaidLocation parity;
};

using Raid5OperationCounts = RaidOperationCounts;

struct Raid5Verification {
    std::uint64_t rowsChecked = 0;
    std::uint64_t parityMismatches = 0;
};

class Raid5 final : public RaidController {
public:
    Raid5(RaidConfiguration configuration, Disks members, RaidWriteJournal journal = {},
          bool interruptedWrite = false,
          UnavailableMembers unavailable = {});

    RaidStatus status() const noexcept override;
    std::vector<RaidLocation> map(std::uint64_t lba) const override;
    Raid5Mapping inspectMapping(std::uint64_t lba) const;
    std::uint64_t rawCapacityBytes() const noexcept;
    std::uint64_t blocksPerRow() const noexcept;
    const Raid5OperationCounts& lastOperation() const noexcept { return counts_; }

    void verify() override;
    void reconstructMemberBlock(std::size_t slot, std::uint64_t physicalLba,
                                std::uint8_t* buffer, std::size_t bufferSize,
                                bool verifySurvivors) override;
    Raid5Verification verifyParity();

private:
    void readBlocksImpl(std::uint64_t lba, std::uint64_t count,
                        std::uint8_t* buffer, std::size_t bufferSize) override;
    void writeBlocksImpl(std::uint64_t lba, std::uint64_t count,
                         const std::uint8_t* buffer, std::size_t bufferSize) override;
    std::size_t parityIndex(std::uint64_t row) const noexcept;
    Raid5Mapping locate(std::uint64_t lba) const noexcept;
    void requireConsistent() const;
    void readPhysical(RaidLocation location, std::uint64_t count, std::uint8_t* buffer);
    void writePhysical(RaidLocation location, std::uint64_t count, const std::uint8_t* buffer);
    void reconstruct(RaidLocation missing, std::uint8_t* buffer);
    void updateBlock(const Raid5Mapping& mapping, const std::uint8_t* buffer, std::vector<bool>& touched);
    void writeFullRow(std::uint64_t row, const std::uint8_t* buffer, std::vector<bool>& touched);

    RaidWriteJournal journal_;
    bool inconsistent_;
    Raid5OperationCounts counts_;
};

}  // namespace raid
