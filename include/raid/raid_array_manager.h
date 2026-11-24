#pragma once

#include "disk/virtual_disk_manager.h"
#include "raid/raid_controller.h"
#include "recovery/failure_manager.h"
#include "recovery/spare_manager.h"

namespace raid {

struct ArrayHealthReport {
    RaidLevel level;
    RaidHealth health;
    std::size_t toleratedFailures;
    bool pendingWrite;
    std::vector<MemberStatus> members;
    bool poolOwned = false;
};

struct ArrayListing {
    RaidConfiguration configuration;
    RaidHealth health;
};

class RaidArrayManager {
public:
    RaidArrayManager(VirtualDiskManager& disks, std::filesystem::path directory);
    RaidArrayManager(const RaidArrayManager&) = delete;
    RaidArrayManager& operator=(const RaidArrayManager&) = delete;

    static bool isValidArrayId(std::string_view id) noexcept;
    RaidController& createArray(std::string_view id, RaidLevel level,
                                std::vector<std::string> members, std::uint64_t stripeUnitBlocks);
    RaidController& openArray(std::string_view id);
    std::vector<RaidConfiguration> listArrays();
    std::vector<ArrayListing> listArraysWithHealth();
    ArrayHealthReport getArrayStatus(std::string_view id);
    bool failMember(std::string_view arrayId, std::string_view memberId);
    void registerSpare(std::string_view id);

    // The catalog remains locked until this manager is destroyed. Call before opening a raw writer.
    void requireUnownedDisk(std::string_view id);
    void closeArrays() noexcept;

private:
    friend class RebuildManager;
    friend class ProvisioningManager;
    using Catalog = std::map<std::string, RaidConfiguration, std::less<>>;
    using Members = std::vector<std::shared_ptr<VirtualDisk>>;

    struct ResolvedMembers {
        Members disks;
        std::vector<MemberStatus> status;
    };

    Catalog loadCatalog();
    void lockCatalog();
    RaidConfiguration findArray(std::string_view id);
    ArrayHealthReport inspectArray(const RaidConfiguration& configuration);
    Members openMembers(const std::vector<std::string>& ids);
    ResolvedMembers resolveMembers(const RaidConfiguration& array, bool acquireHandles);
    bool hasPendingWrite(const RaidConfiguration& array) const;
    bool poolOwns(std::string_view id) const;
    std::unique_ptr<RaidController> makeController(RaidConfiguration configuration, Members members,
                                                   std::vector<bool> unavailable = {});

    VirtualDiskManager& disks_;
    std::filesystem::path directory_;
    FailureManager failures_;
    SpareManager spares_;
    std::shared_ptr<void> catalogLock_;
    std::map<std::string, std::unique_ptr<RaidController>, std::less<>> arrays_;
};

}  // namespace raid
