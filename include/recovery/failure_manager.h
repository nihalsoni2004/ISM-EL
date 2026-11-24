#pragma once

#include "raid/raid_types.h"
#include "recovery/disk_state.h"

#include <filesystem>

namespace raid {

struct FailureState {
    std::vector<MemberFailure> members;
    bool persisted = false;
    std::vector<std::string> activeMembers = {};
    std::vector<std::string> activeIdentities = {};
    std::vector<std::string> completedJobs = {};
};

// Access is serialized by RaidArrayManager's catalog lock.
class FailureManager {
public:
    explicit FailureManager(std::filesystem::path directory);

    FailureState load(const RaidConfiguration& array) const;
    bool failMember(const RaidConfiguration& array, FailureState& state, std::size_t index,
                    FailureCause reason) const;
    void promote(const RaidConfiguration& array, std::size_t slot, std::string spare,
                 std::vector<std::string> identities, std::string job) const;

private:
    std::filesystem::path directory_;
};

}  // namespace raid
