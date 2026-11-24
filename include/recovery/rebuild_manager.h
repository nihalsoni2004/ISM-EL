#pragma once

#include "recovery/rebuild_job.h"
#include "recovery/spare_manager.h"
#include "recovery/disk_state.h"

#include <functional>

namespace raid {

class RaidArrayManager;
class RaidController;
class StorageEngine;

struct SpareListing {
    SpareRecord disk;
    DiskState state;
    std::string assignment;
    bool available;
};

enum class RebuildPhase { Start, SourceRead, SpareWrite, SpareFlush, PublishCheckpoint, CheckpointSaved,
                          Verify, Promote, PromotionCommitted };

class RebuildManager {
public:
    using PhaseObserver = std::function<void(RebuildPhase)>;
    using StopRequested = std::function<bool()>;
    using TraceObserver = std::function<void(const scheduling::TraceEvent&)>;
    explicit RebuildManager(std::filesystem::path root, std::uint64_t batchBlocks = 16,
                            PhaseObserver observer = {}, TraceObserver trace = {});
    std::vector<SpareListing> listSpares();
    RebuildJob prepare(std::string_view array, std::string_view failed, std::string_view spare,
                       scheduling::Config policy = {});
    RebuildJob status(std::string_view id);
    RebuildJob pause(std::string_view id);
    std::vector<RebuildJob> list();
    std::vector<RebuildJob> listForArray(RaidArrayManager& arrays, std::string_view id);
    RebuildJob run(std::string_view id, std::uint64_t checkpointLimit = 0, StopRequested stop = {});

private:
    RebuildJob reconcile(RaidArrayManager& arrays, RebuildJob job, bool detectInterruption) const;
    RaidController& validateSources(StorageEngine& engine, const RebuildJob& job) const;
    RebuildJob runWorker(std::string_view id, std::uint64_t checkpointLimit, const StopRequested& stop);
    void observe(RebuildPhase phase) const;

    std::filesystem::path root_;
    RebuildJobStore jobs_;
    std::uint64_t batchBlocks_;
    PhaseObserver observer_;
    TraceObserver trace_;
};

}  // namespace raid
