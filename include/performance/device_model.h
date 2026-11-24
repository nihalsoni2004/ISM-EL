#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace raid::performance {

enum class Operation { Read, Write };
enum class Access { Sequential, Random };

inline constexpr std::size_t maxRequests = 64;
inline constexpr std::size_t maxDevices = 64;
inline constexpr std::size_t maxDependencies = 8;
inline constexpr std::uint64_t maxBytes = 64 * 1024 * 1024;
inline constexpr double maxArrivalMs = 86400000;
inline constexpr double minBandwidth = 0.001;
inline constexpr double maxBandwidth = 1000000;
inline constexpr double maxLatencyMs = 60000;
inline constexpr std::size_t maxQueueDepth = 32;

struct Parameters {
    double sequentialReadMs;
    double sequentialWriteMs;
    double randomReadMs;
    double randomWriteMs;
    double readMiBps;
    double writeMiBps;
    std::size_t queueDepth;
};

struct Profile {
    std::string_view id;
    Parameters settings;
};

const std::array<Profile, 3>& profiles() noexcept;
const Profile& profile(std::string_view id);
void validate(const Parameters& settings);
double estimate(const Parameters& settings, Operation operation, Access access, std::uint64_t bytes);

struct Request {
    std::size_t device;
    double arrivalMs;
    Operation operation;
    std::uint64_t bytes;
    Access access;
    std::vector<std::size_t> dependencies;
};
struct Result {
    double arrivalMs = 0;
    double startMs = 0;
    double serviceMs = 0;
    double queueWaitMs = 0;
    double completionMs = 0;
};
struct Scenario {
    std::vector<Result> requests;
    double firstArrivalMs;
    double lastCompletionMs;
    double elapsedMs() const noexcept { return lastCompletionMs - firstArrivalMs; }
};

Scenario simulate(const Parameters& settings, std::size_t devices, const std::vector<Request>& requests);

}  // namespace raid::performance
