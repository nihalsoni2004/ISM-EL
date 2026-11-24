#pragma once

#include "benchmark/benchmark.h"
#include "recovery/scheduling/scheduler.h"

namespace raid::benchmark {

struct LoadPhase { std::uint64_t durationMs, rate; };
struct LoadProfile { std::string id; std::vector<LoadPhase> phases; };
struct Study {
    std::uint64_t members, diskMiB, batchBlocks, repetitions, seed, runtimeSeconds;
    RaidLevel level;
    scheduling::Config scheduling;
    std::vector<std::string> conditions;
    std::vector<LoadProfile> profiles;
    Json normalized;
};
Study readStudy(const fs::path& path);

struct ReadSample {
    std::uint64_t scheduledNs = 0, lba = 0;
    std::optional<std::uint64_t> submitNs, operationStartNs, operationEndNs, completionNs;
    std::optional<bool> validated;
    bool success = false, active = false;
    std::string error;
};
struct StudyTrial {
    std::vector<ReadSample> reads;
    std::vector<scheduling::TraceEvent> traces;
    std::uint64_t windowNs = 0;
    std::optional<std::uint64_t> rebuildNs;
    std::string jobId, jobState, health, error;
    bool rebuildValidated = false, sourceUnchanged = false, foregroundValidated = false, passed = false;
};
StudyTrial runTrial(const Study& study, std::string_view condition, const LoadProfile& load,
                    std::uint64_t repetition, const fs::path& root, telemetry::Clock::time_point deadline);
RunResult runStudy(const Study& study, const fs::path& root);

}  // namespace raid::benchmark
