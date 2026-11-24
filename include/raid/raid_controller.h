#pragma once

#include "disk/virtual_disk.h"
#include "raid/raid_types.h"

#include <functional>
#include <string_view>

namespace raid {

struct RaidWriteJournal {
    std::function<void()> begin;
    std::function<void()> complete;
};

class RaidController {
public:
    virtual ~RaidController() = default;
    RaidController(const RaidController&) = delete;
    RaidController& operator=(const RaidController&) = delete;

    RaidLevel level() const noexcept { return configuration_.level; }
    std::uint64_t capacityBlocks() const noexcept { return configuration_.capacityBlocks; }
    const RaidConfiguration& configuration() const noexcept { return configuration_; }

    virtual RaidStatus status() const noexcept = 0;
    virtual std::vector<RaidLocation> map(std::uint64_t lba) const = 0;
    void readBlocks(std::uint64_t lba, std::uint64_t count, std::uint8_t* buffer, std::size_t bufferSize);
    void writeBlocks(std::uint64_t lba, std::uint64_t count, const std::uint8_t* buffer, std::size_t bufferSize);
    bool poolOwned() const noexcept { return poolOwned_; }
    virtual void verify() = 0;
    virtual void reconstructMemberBlock(std::size_t slot, std::uint64_t physicalLba,
                                        std::uint8_t* buffer, std::size_t bufferSize,
                                        bool verifySurvivors);

    void markUnavailable(std::string_view id);
    bool memberAvailable(std::size_t index) const noexcept;

protected:
    using Disks = std::vector<std::shared_ptr<VirtualDisk>>;
    using UnavailableMembers = std::vector<bool>;

    RaidController(RaidConfiguration configuration, Disks members, RaidLevel expectedLevel,
                   UnavailableMembers unavailable);
    VirtualDisk& member(std::size_t index) const noexcept { return *members_[index]; }
    std::size_t memberCount() const noexcept { return members_.size(); }
    bool available(std::size_t index) const noexcept;
    std::size_t availableCount() const noexcept;
    RaidHealth health(bool inconsistent = false) const noexcept;
    void validateRebuildBlock(std::size_t slot, std::uint64_t physicalLba,
                              const std::uint8_t* buffer, std::size_t bufferSize) const;
    void markUnavailable(std::size_t index) noexcept { unavailable_[index] = true; }

    void validateRange(std::uint64_t lba, std::uint64_t count) const;
    std::size_t validateTransfer(std::uint64_t lba, std::uint64_t count,
                                 const void* buffer, std::size_t bufferSize) const;

private:
    friend class RaidArrayManager;
    friend class ProvisioningManager;

    virtual void readBlocksImpl(std::uint64_t lba, std::uint64_t count,
                                std::uint8_t* buffer, std::size_t bufferSize) = 0;
    virtual void writeBlocksImpl(std::uint64_t lba, std::uint64_t count,
                                 const std::uint8_t* buffer, std::size_t bufferSize) = 0;
    void writePoolBlocks(std::uint64_t lba, std::uint64_t count,
                         const std::uint8_t* buffer, std::size_t bufferSize);

    RaidConfiguration configuration_;
    Disks members_;  // The disk manager outlives its controllers and keeps these open.
    std::vector<bool> unavailable_;
    bool poolOwned_ = false;
};

}  // namespace raid
