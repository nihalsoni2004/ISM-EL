#pragma once

#include "raid/raid_types.h"
#include "recovery/scheduling/scheduler.h"

#include <filesystem>
#include <memory>
#include <optional>

namespace raid {

enum class RebuildStatus { Prepared, Running, Paused, Interrupted, Completed, Failed };

std::string_view rebuildStatusName(RebuildStatus status) noexcept;
bool isRebuildId(std::string_view id) noexcept;

struct RebuildJob {
    std::string id;
    std::string arrayId;
    std::string failedMember;
    std::string spareMember;
    std::string spareIdentity;
    std::uint64_t slot = 0;
    RaidLevel level = RaidLevel::Raid0;
    std::uint64_t stripe = 0;
    std::string layout;
    std::vector<std::string> members;
    std::vector<std::string> sources;  // Identity:revision for trusted survivors, "-" for unavailable members.
    RebuildStatus status = RebuildStatus::Prepared;
    std::uint64_t totalBlocks = 0;
    std::uint64_t batchBlocks = 16;
    std::uint64_t completedBlocks = 0;
    bool pauseRequested = false;
    std::uint64_t started = 0;
    std::uint64_t checkpoint = 0;
    std::string lastError;
    scheduling::Config scheduling;
};

class RebuildJobStore {
public:
    explicit RebuildJobStore(std::filesystem::path directory);
    RebuildJob load(std::string_view id) const;
    std::vector<RebuildJob> list() const;
    void save(const RebuildJob& job, bool create = false) const;
    std::shared_ptr<void> tryLease(std::string_view id) const;

private:
    std::filesystem::path directory_;
};

}  // namespace raid
