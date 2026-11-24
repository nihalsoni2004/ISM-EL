#pragma once

#include "raid/raid_controller.h"

namespace raid {

using MirrorWriteJournal = RaidWriteJournal;

class Raid1 final : public RaidController {
public:
    Raid1(RaidConfiguration configuration, Disks members, MirrorWriteJournal journal = {},
          bool interruptedWrite = false,
          UnavailableMembers unavailable = {});

    RaidStatus status() const noexcept override;
    std::vector<RaidLocation> map(std::uint64_t lba) const override;
    void verify() override;
    void reconstructMemberBlock(std::size_t slot, std::uint64_t physicalLba,
                                std::uint8_t* buffer, std::size_t bufferSize,
                                bool verifySurvivors) override;

private:
    void readBlocksImpl(std::uint64_t lba, std::uint64_t count,
                        std::uint8_t* buffer, std::size_t bufferSize) override;
    void writeBlocksImpl(std::uint64_t lba, std::uint64_t count,
                         const std::uint8_t* buffer, std::size_t bufferSize) override;
    void requireConsistent() const;
    MirrorWriteJournal journal_;
    bool inconsistent_;
};

}  // namespace raid
