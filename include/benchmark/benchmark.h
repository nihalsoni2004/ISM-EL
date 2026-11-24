#pragma once

#include "raid/raid_types.h"
#include "telemetry/telemetry.h"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace raid::benchmark {

using Json = nlohmann::json;
namespace fs = std::filesystem;
inline constexpr std::uint64_t mib = 1024 * 1024;
inline constexpr std::uint64_t chunkBlocks = 16;

enum class Workload { RandomRead, RandomWrite, SequentialRead, SequentialWrite, Mixed, ThinFirst, ThinOverwrite, ReadBaseline };
enum class Validation { Full, Sampled, None };
enum class CapacityMode { PerMember, EqualRaw, EqualUsable };

struct Config {
    std::string name;
    RaidLevel level;
    Workload workload;
    Validation validation;
    CapacityMode capacityMode;
    std::uint64_t disks, diskMiB, stripe, footprint, blocks, operations, warmup, repetitions, seed;
    std::uint64_t readPercent, thinOffset, sampleStride, timeoutSeconds, maxRawBytes;
    std::uint64_t maxImageMiB, maxDisks, maxOperations;
    std::uint64_t targetMiB;
    bool rebuild;
};

struct Operation {
    bool write;
    std::uint64_t lba;
    std::uint64_t blocks;
    std::uint64_t pattern;
};

std::string_view name(Workload value) noexcept;
std::string_view name(Validation value) noexcept;
std::string_view name(CapacityMode value) noexcept;
bool thin(Workload value) noexcept;
Config parseConfig(const Json& input);
Config loadConfig(const fs::path& path);
Json describe(const Config& config);
std::vector<Operation> makeWorkload(const Config& config, std::uint64_t repetition);
void fillBlock(std::uint8_t* out, std::uint64_t seed, std::uint64_t lba, std::uint64_t pattern) noexcept;
std::uint64_t usableBlocks(const Config& config);
std::uint64_t imageBudgetMiB(const Config& config) noexcept;

struct Sample {
    Operation operation;
    telemetry::PhysicalCounts physical;
    std::uint64_t durationNs = 0;
    std::uint64_t newChunks = 0;
    bool success = false;
    std::optional<bool> validation;
    bool rebuildOverlap = false;
    std::string error;
};

struct Repetition {
    std::vector<Sample> samples;
    double elapsed = 0;
    double preparation = 0;
    double warmup = 0;
    std::optional<std::uint64_t> allocatedBefore, allocatedAfter;
    std::optional<double> rebuildSeconds;
    std::uint64_t rebuilt = 0, rebuildTotal = 0;
    std::string rebuildState;
    std::string error;
    bool persisted = false;
};

Repetition measure(const Config& config, std::uint64_t repetition,
                    const fs::path& root, telemetry::Clock::time_point deadline);
bool valid(const Config& config, const Repetition& repetition) noexcept;
Json summarizeRun(const Config& config, const Repetition& repetition);
std::string escapeCsv(std::string_view value);

struct RunResult {
    fs::path directory;
    bool passed;
};

fs::path outputRoot(const fs::path& benchmarkRoot, std::string_view group);
bool safeName(std::string_view value) noexcept;
RunResult run(const Config& config, const fs::path& benchmarkRoot, std::string_view group = {},
              const Json& invocation = Json::object());
Json info(const fs::path& benchmarkRoot, std::string_view group, std::string_view runId);

}  // namespace raid::benchmark
