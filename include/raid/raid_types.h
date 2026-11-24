#pragma once

#include "core/config.h"
#include "recovery/raid_health.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace raid {

enum class RaidLevel { Raid0 = 0, Raid1 = 1, Raid5 = 5, Raid6 = 6 };

inline constexpr std::uint64_t raidSchemaVersion = 1;
inline constexpr std::uint64_t raidBlockSize = config::defaultLogicalBlockSizeBytes;
inline constexpr std::uint64_t defaultStripeUnitBlocks = 16;
inline constexpr std::uint64_t mirrorStripeUnitBlocks = 1;
inline constexpr std::string_view raid5Layout = "left_asymmetric_v1";
inline constexpr std::string_view raid6Layout = "rotating_pq_ascending_data_v1";
inline constexpr std::size_t maxRaid6Members = 257;

constexpr std::string_view parityLayout(RaidLevel level) noexcept {
    switch (level) {
    case RaidLevel::Raid5: return raid5Layout;
    case RaidLevel::Raid6: return raid6Layout;
    default: return {};
    }
}

// The caller validates the level and member count before using this geometry.
constexpr std::size_t dataUnitCount(RaidLevel level, std::size_t members) noexcept {
    switch (level) {
    case RaidLevel::Raid1: return 1;
    case RaidLevel::Raid5: return members - 1;
    case RaidLevel::Raid6: return members - 2;
    default: return members;
    }
}

struct RaidOperationCounts {
    std::uint64_t logicalBlocks = 0;
    std::uint64_t physicalBlockReads = 0;
    std::uint64_t physicalBlockWrites = 0;
    std::uint64_t readRequests = 0;
    std::uint64_t writeRequests = 0;
    std::uint64_t fullStripeRows = 0;
    std::uint64_t partialStripeRows = 0;
};

struct RaidConfiguration {
    std::string id;
    RaidLevel level;
    std::vector<std::string> members;  // Order determines stripe placement.
    std::uint64_t stripeUnitBlocks;
    std::uint64_t capacityBlocks;
    std::uint64_t blockSize = raidBlockSize;
    std::uint64_t schemaVersion = raidSchemaVersion;
    std::string layout = {};
    std::vector<std::string> memberIdentities = {};
    std::vector<std::string> originalMembers = {};
    std::vector<std::string> completedRebuilds = {};
};

struct RaidLocation {
    std::size_t memberIndex;
    std::uint64_t physicalLba;
};

std::uint64_t calculateRaidCapacity(RaidLevel level, std::size_t memberCount,
                                    std::uint64_t memberBlocks, std::uint64_t stripeUnitBlocks);
void validateRaidLayout(RaidLevel level, std::size_t memberCount, std::uint64_t stripeUnitBlocks);

}  // namespace raid
