#pragma once

#include "disk/virtual_disk_manager.h"
#include "raid/raid_array_manager.h"

#include <optional>
#include <string_view>

namespace raid {

enum class EngineState { Created, Initialized, Shutdown };

struct EngineStatus {
    EngineState state;
};

class StorageEngine {
public:
    StorageEngine() noexcept = default;
    StorageEngine(const StorageEngine&) = delete;
    StorageEngine& operator=(const StorageEngine&) = delete;

    // Return false for an invalid transition. Filesystem failures are reported by exceptions.
    bool initialize();
    bool initialize(const std::filesystem::path& projectRoot);
    bool shutdown();

    std::string_view getVersion() const noexcept;
    EngineStatus getStatus() const noexcept;
    VirtualDiskManager& disks();
    RaidArrayManager& arrays();

private:
    EngineState state_ = EngineState::Created;
    std::optional<VirtualDiskManager> disks_;
    std::optional<RaidArrayManager> arrays_;
};

}  // namespace raid
