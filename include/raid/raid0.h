#pragma once

#include "raid/raid_controller.h"

namespace raid {

class Raid0 final : public RaidController {
public:
    Raid0(RaidConfiguration configuration, Disks members, UnavailableMembers unavailable = {},
          bool interruptedWrite = false);

    RaidStatus status() const noexcept override;
    std::vector<RaidLocation> map(std::uint64_t lba) const override;
    void verify() override;

private:
    void readBlocksImpl(std::uint64_t lba, std::uint64_t count,
                        std::uint8_t* buffer, std::size_t bufferSize) override;
    void writeBlocksImpl(std::uint64_t lba, std::uint64_t count,
                         const std::uint8_t* buffer, std::size_t bufferSize) override;
    bool inconsistent_;
    RaidLocation locate(std::uint64_t lba) const noexcept;
    void transfer(std::uint64_t lba, std::uint64_t count,
                  const std::function<void(VirtualDisk&, std::uint64_t, std::uint64_t, std::size_t)>& io);
};

}  // namespace raid
