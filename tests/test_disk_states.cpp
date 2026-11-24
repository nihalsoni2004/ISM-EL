#include "raid/raid_types.h"
#include "recovery/disk_state.h"
#include "test_support.h"

#include <array>
#include <iostream>

int main() {
    try {
        using namespace raid;
        using test::require;
        const MemberFailure member;
        require(member.state == DiskState::Online && member.cause == FailureCause::None, "default member state");
        require(diskStateName(DiskState::Spare) == "SPARE" &&
                diskStateName(DiskState::Rebuilding) == "REBUILDING", "reserved member states");
        struct Case {
            RaidLevel level;
            std::size_t tolerated;
        };
        const std::array<Case, 4> cases{{{RaidLevel::Raid0, 0}, {RaidLevel::Raid1, 1},
                                         {RaidLevel::Raid5, 1}, {RaidLevel::Raid6, 2}}};
        for (const auto& entry : cases) {
            require(failureTolerance(entry.level) == entry.tolerated, "wrong tolerance");
            for (std::size_t unavailable = 0; unavailable <= 4; ++unavailable) {
                auto expected = RaidHealth::Healthy;
                if (unavailable != 0) {
                    expected = unavailable > entry.tolerated ? RaidHealth::Failed : RaidHealth::Degraded;
                }
                const auto health = calculateRaidHealth(entry.level, unavailable, false);
                require(health == expected && readsPermitted(health) == (unavailable <= entry.tolerated) &&
                        writesPermitted(health) == (unavailable == 0), "incorrect effective health or I/O");
                const auto inconsistent = calculateRaidHealth(entry.level, unavailable, true);
                require(inconsistent == RaidHealth::Inconsistent && !readsPermitted(inconsistent) &&
                        !writesPermitted(inconsistent), "pending write did not override availability");
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
