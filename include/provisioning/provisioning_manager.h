#pragma once

#include "raid/raid_types.h"

#include <functional>
#include <optional>

namespace raid {

struct PoolOptions {
    std::uint64_t chunkBlocks = config::defaultThinProvisioningChunkSizeBytes / raidBlockSize;
    std::uint64_t warning = 70;
    std::uint64_t critical = 90;
    std::uint64_t maxProvisioningRatio = 0;  // Zero means unlimited.
};

enum class CapacityState { Normal, Warning, Critical, Full };
std::string_view capacityStateName(CapacityState state) noexcept;

struct PoolInfo {
    std::string id;
    std::string arrayId;
    PoolOptions options;
    std::uint64_t raidBlocks;
    std::uint64_t totalChunks;
    std::uint64_t allocatedChunks;
    std::uint64_t reservedChunks;
    std::uint64_t freeChunks;
    std::uint64_t provisionedBlocks;
    CapacityState capacityState;
    RaidHealth health;
};

struct LunInfo {
    std::string id;
    std::string poolId;
    std::uint64_t blocks;
    std::uint64_t chunkBlocks;
    std::uint64_t allocatedChunks;
    bool fileManaged = false;
};

struct LunMapping {
    std::uint64_t logicalChunk;
    std::uint64_t offset;
    std::optional<std::uint64_t> physicalChunk;
    std::optional<std::uint64_t> raidLba;
};

struct LunWriteResult {
    std::uint64_t newChunks;
    std::uint64_t allocatedChunks;
    std::uint64_t chunkBlocks;
    std::uint64_t firstPhysicalChunk;
};

enum class AllocationPhase { BeforeReservation, Reserved, BeforeChunkWrite, ChunkWritten, BeforeCommit, Committed };

class ProvisioningManager {
public:
    using PhaseObserver = std::function<void(AllocationPhase)>;
    explicit ProvisioningManager(std::filesystem::path root, PhaseObserver observer = {});

    PoolInfo createPool(std::string_view id, std::string_view array, PoolOptions options = {});
    PoolInfo poolInfo(std::string_view id);
    std::vector<PoolInfo> listPools();
    PoolInfo setWatermarks(std::string_view id, std::uint64_t warning, std::uint64_t critical);
    PoolInfo verifyPool(std::string_view id);

    LunInfo createLun(std::string_view id, std::string_view pool, std::uint64_t capacityBytes);
    LunInfo lunInfo(std::string_view id);
    std::vector<LunInfo> listLuns();
    LunMapping map(std::string_view id, std::uint64_t lba);
    LunMapping readBlocks(std::string_view id, std::uint64_t lba, std::uint64_t count,
                          std::uint8_t* buffer, std::size_t bufferSize);
    LunWriteResult writeBlocks(std::string_view id, std::uint64_t lba, std::uint64_t count,
                               const std::uint8_t* buffer, std::size_t bufferSize);

private:
    friend class FileManager;
    struct Session;
    LunInfo createLunImpl(std::string_view id, std::string_view pool, std::uint64_t capacityBytes,
                          std::string_view fileCatalog);
    LunInfo fileLun(std::string_view id, std::string_view fileCatalog, std::optional<bool> writing);
    LunWriteResult writeBlocksImpl(std::string_view id, std::uint64_t lba, std::uint64_t count,
                                   const std::uint8_t* buffer, std::size_t bufferSize,
                                   std::string_view fileCatalog);
    void observe(AllocationPhase phase) const;
    std::filesystem::path root_;
    PhaseObserver observer_;
};

}  // namespace raid
