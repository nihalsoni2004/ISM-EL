#pragma once

#include <cstddef>
#include <string_view>

namespace raid {

enum class RaidLevel;
enum class RaidHealth { Healthy, Degraded, Failed, Inconsistent, Unavailable = Failed };
using RaidStatus = RaidHealth;  // Existing controller API.

std::size_t failureTolerance(RaidLevel level) noexcept;
RaidHealth calculateRaidHealth(RaidLevel level, std::size_t unavailable, bool inconsistent) noexcept;

constexpr std::string_view raidHealthName(RaidHealth health) noexcept {
    switch (health) {
    case RaidHealth::Healthy: return "HEALTHY";
    case RaidHealth::Degraded: return "DEGRADED";
    case RaidHealth::Failed: return "FAILED";
    case RaidHealth::Inconsistent: return "INCONSISTENT";
    }
    return "UNKNOWN";
}

constexpr bool readsPermitted(RaidHealth health) noexcept {
    return health == RaidHealth::Healthy || health == RaidHealth::Degraded;
}

constexpr bool writesPermitted(RaidHealth health) noexcept {
    return health == RaidHealth::Healthy;
}

}  // namespace raid
