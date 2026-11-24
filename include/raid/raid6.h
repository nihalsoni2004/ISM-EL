#pragma once

#include "raid/raid_controller.h"

namespace raid {

struct Raid6Mapping {
    std::string_view arrayId;  // Refers to the controller's configuration.
    std::uint64_t logicalLba;
    std::uint64_t stripeRow;
    std::size_t dataUnitIndex;
    RaidLocation data;
    RaidLocation p;
    RaidLocation q;
};

struct Raid6Verification {
    std::uint64_t rowsChecked = 0;
    std::uint64_t pMismatches = 0;
    std::uint64_t qMismatches = 0;
    std::uint64_t rowsWithErrors = 0;
};

class Raid6 final : public RaidController {
public:
    using MemberFlush = std::function<void(VirtualDisk&)>;
    Raid6(RaidConfiguration configuration, Disks members, RaidWriteJournal journal = {},
          bool interruptedWrite = false, MemberFlush flushMember = &VirtualDisk::flush,
          UnavailableMembers unavailable = {});

    RaidStatus status() const noexcept override;
    std::vector<RaidLocation> map(std::uint64_t lba) const override;
    Raid6Mapping inspectMapping(std::uint64_t lba) const;
    std::uint64_t rawCapacityBytes() const noexcept;
    std::uint64_t blocksPerRow() const noexcept;
    const RaidOperationCounts& lastOperation() const noexcept { return counts_; }

    void verify() override;
    void reconstructMemberBlock(std::size_t slot, std::uint64_t physicalLba,
                                std::uint8_t* buffer, std::size_t bufferSize,
                                bool verifySurvivors) override;
    Raid6Verification verifyParity();

private:
    void readBlocksImpl(std::uint64_t lba, std::uint64_t count,
                        std::uint8_t* buffer, std::size_t bufferSize) override;
    void writeBlocksImpl(std::uint64_t lba, std::uint64_t count,
                         const std::uint8_t* buffer, std::size_t bufferSize) override;
    struct ParityMembers {
        std::size_t p;
        std::size_t q;

        std::size_t dataMember(std::size_t index) const noexcept;
    };

    ParityMembers parityMembers(std::uint64_t row) const noexcept;
    Raid6Mapping locate(std::uint64_t lba) const noexcept;
    void requireConsistent() const;
    void readPhysical(RaidLocation location, std::uint64_t count, std::uint8_t* buffer);
    void writePhysical(RaidLocation location, std::uint64_t count, const std::uint8_t* buffer);
    void reconstruct(const Raid6Mapping& mapping, std::uint64_t offset, std::uint8_t* buffer);
    void updateBlock(const Raid6Mapping& mapping, const std::uint8_t* buffer, std::vector<bool>& touched);
    void writeFullRow(std::uint64_t row, const std::uint8_t* buffer, std::vector<bool>& touched);

    RaidWriteJournal journal_;
    MemberFlush flushMember_;
    bool inconsistent_;
    RaidOperationCounts counts_;
};

}  // namespace raid
