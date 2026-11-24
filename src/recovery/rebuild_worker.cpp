#include "recovery/rebuild_manager.h"

#include "core/engine.h"
#include "record.h"

#include <algorithm>
#include <future>
#include <thread>

namespace raid {
namespace {

std::uint64_t elapsedNs(scheduling::Clock::duration duration) noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

void validateSpare(const VirtualDisk& spare, const RebuildJob& job) {
    if (spare.getBlockCount() != job.totalBlocks || spare.imageIdentity() != job.spareIdentity) {
        throw std::runtime_error("Replacement spare identity or capacity changed");
    }
}

}  // namespace

RebuildJob RebuildManager::run(std::string_view id, std::uint64_t checkpointLimit, StopRequested stop) {
    const auto existing = status(id);
    if (existing.status == RebuildStatus::Completed || existing.status == RebuildStatus::Failed) {
        throw std::invalid_argument("Completed or failed rebuild cannot be run");
    }
    std::packaged_task<RebuildJob()> work([this, jobId = std::string(id), checkpointLimit, stop = std::move(stop)] {
        return runWorker(jobId, checkpointLimit, stop);
    });
    auto result = work.get_future();
    std::thread worker(std::move(work));
    worker.join();
    return result.get();
}

RebuildJob RebuildManager::runWorker(std::string_view id, std::uint64_t checkpointLimit, const StopRequested& stop) {
    const auto lease = jobs_.tryLease(id);
    if (!lease) {
        throw std::runtime_error("A worker already holds this rebuild job");
    }

    VirtualDisk spare;
    bool started = false;
    std::uint64_t batchSize = 0, completed = 0, totalBlocks = 0;
    scheduling::Config policy;
    scheduling::Clock::time_point activated;
    try {
        {
            StorageEngine engine;
            engine.initialize(root_);
            auto& arrays = engine.arrays();
            arrays.lockCatalog();
            auto job = reconcile(arrays, jobs_.load(id), false);
            if (job.status == RebuildStatus::Completed || job.status == RebuildStatus::Failed) {
                throw std::invalid_argument("Completed or failed rebuild cannot be run");
            }
            const bool firstStart = job.status == RebuildStatus::Prepared;
            if (firstStart) {
                // Check the blank-image policy before publishing a resumable RUNNING record.
                spare.open(engine.disks().getDiskInfo(job.spareMember).path);
                validateSpare(spare, job);
                const auto registered = arrays.spares_.find(job.spareMember);
                if (!registered) {
                    throw std::runtime_error("Replacement spare registration is missing");
                }
                SpareManager::validateAvailable(*registered, spare);
                SpareManager::validateBlank(spare);
            }
            job.status = RebuildStatus::Running;
            job.pauseRequested = false;
            job.lastError.clear();
            if (job.started == 0) {
                job.started = record::now();
            }
            observe(RebuildPhase::Start);
            jobs_.save(job);
            activated = scheduling::Clock::now();
            started = true;
            completed = job.completedBlocks;
            totalBlocks = job.totalBlocks;
            policy = job.scheduling;
            validateSources(engine, job);
            if (!firstStart) {
                spare.open(engine.disks().getDiskInfo(job.spareMember).path);
                validateSpare(spare, job);
            }
            batchSize = job.batchBlocks;
        }

        auto stamp = [&](scheduling::TraceEvent event) noexcept {
            if (!trace_) return;
            event.at = scheduling::Clock::now();
            event.elapsedNs = elapsedNs(event.at - activated);
            try { trace_(event); } catch (...) { /* Traces are observational. */ }
        };
        using Kind = scheduling::TraceEvent::Kind;
        scheduling::Controller controller(policy);
        scheduling::TraceEvent initial;
        initial.kind = Kind::Started;
        initial.completedBlocks = completed;
        initial.totalBlocks = totalBlocks;
        stamp(initial);
        scheduling::Load load;

        std::vector<std::uint8_t> expected(batchSize * raidBlockSize);
        auto actual = expected;
        std::uint64_t verified = 0;
        std::uint64_t completedBatches = 0;
        bool subsequent = false;
        for (;;) {
            scheduling::TraceEvent metrics;
            auto now = scheduling::Clock::now();
            const auto t = elapsedNs(now - activated) / 1000000;
            if (controller.due(t)) {
                if (policy.policy == scheduling::Policy::Adaptive) {
                    try { load = scheduling::observe(policy.windowMs); }
                    catch (...) { load = {}; load.outstanding = 1; }
                }
                const auto updated = controller.step(t, load);
                if (trace_) {
                    scheduling::TraceEvent update;
                    update.kind = Kind::Decision;
                    update.completedBlocks = completed;
                    update.totalBlocks = totalBlocks;
                    update.decision = updated;
                    update.load = load;
                    update.controllerNs = elapsedNs(scheduling::Clock::now() - now);
                    stamp(update);
                }
            }
            const auto delay = controller.current().delayUs;
            if (subsequent && delay && !(checkpointLimit && completedBatches >= checkpointLimit)) {
                const auto waitStart = scheduling::Clock::now();
                scheduling::waitInterruptibly(std::chrono::microseconds(delay), [&] {
                    if (stop && stop()) return true;
                    try { return jobs_.load(id).pauseRequested; }
                    catch (...) { return false; } // The locked read below remains authoritative.
                });
                metrics.waitNs = elapsedNs(scheduling::Clock::now() - waitStart);
            }
            bool checkpointSaved = false;
            std::optional<RebuildJob> ending;
            scheduling::Clock::time_point acquired;
            {
                // The preceding pacing phase holds no catalog lock. Checkpoint and promotion remain inside this scope.
                StorageEngine engine;
                engine.initialize(root_);
                auto& arrays = engine.arrays();
                arrays.lockCatalog();
                acquired = scheduling::Clock::now();
                auto job = jobs_.load(id);
                if (job.status != RebuildStatus::Running) {
                    throw std::runtime_error("Rebuild job state changed while holding its worker lease");
                }
                if (job.pauseRequested || (stop && stop()) ||
                    (checkpointLimit != 0 && completedBatches >= checkpointLimit)) {
                    job.status = RebuildStatus::Paused;
                    job.pauseRequested = false;
                    jobs_.save(job);
                    metrics.kind = Kind::Paused;
                    ending = std::move(job);
                } else {
                    auto& array = validateSources(engine, job);
                    const bool verifying = job.completedBlocks == job.totalBlocks;
                    const auto offset = verifying ? verified : job.completedBlocks;
                    const auto count = std::min(batchSize, job.totalBlocks - offset);
                    const auto bytes = static_cast<std::size_t>(count * raidBlockSize);
                    const telemetry::Scope scope(telemetry::Origin::Rebuild, job.arrayId, job.id);
                    telemetry::PhysicalCapture capture(true);
                    const auto processing = scheduling::Clock::now();
                    telemetry::OperationTimer timer(verifying ? telemetry::Kind::RebuildVerify : telemetry::Kind::RebuildBatch,
                                                    count, raidBlockSize);
                    metrics.kind = verifying ? Kind::Verification : Kind::Reconstruction;
                    observe(verifying ? RebuildPhase::Verify : RebuildPhase::SourceRead);
                    for (std::uint64_t block = 0; block < count; ++block) {
                        array.reconstructMemberBlock(static_cast<std::size_t>(job.slot), offset + block,
                                                     expected.data() + block * raidBlockSize, raidBlockSize, verifying);
                    }
                    if (!verifying) {
                        observe(RebuildPhase::SpareWrite);
                        spare.writeBlocks(offset, count, expected.data(), bytes);
                        observe(RebuildPhase::SpareFlush);
                        spare.flush();
                    }
                    spare.readBlocks(offset, count, actual.data(), bytes);
                    if (!std::equal(expected.data(), expected.data() + bytes, actual.data())) {
                        throw std::runtime_error("Replacement verification failed at batch starting at block " +
                                                 std::to_string(offset));
                    }
                    if (!verifying) {
                        job.completedBlocks += count;
                        job.checkpoint = record::now();
                        observe(RebuildPhase::PublishCheckpoint);
                        const auto checkpointStart = scheduling::Clock::now();
                        jobs_.save(job);
                        metrics.checkpointNs = elapsedNs(scheduling::Clock::now() - checkpointStart);
                        completed = job.completedBlocks;
                        ++completedBatches;
                        checkpointSaved = true;
                    } else {
                        verified += count;
                        if (verified == job.totalBlocks) {
                            spare.flush();
                            validateSpare(spare, job);
                            std::vector<std::string> identities;
                            for (const auto& source : job.sources)
                                identities.push_back(source == "-" ? "-" : source.substr(0, 64));
                            identities[job.slot] = job.spareIdentity;
                            observe(RebuildPhase::Promote);
                            arrays.failures_.promote(array.configuration(), static_cast<std::size_t>(job.slot),
                                                     job.spareMember, std::move(identities), job.id);
                            spare.close();
                            observe(RebuildPhase::PromotionCommitted);
                            // The state record is the commit point. Status can reconcile an interrupted job update.
                            job.status = RebuildStatus::Completed;
                            jobs_.save(job);
                            metrics.kind = Kind::Completed;
                            ending = std::move(job);
                        }
                    }
                    timer.success();
                    metrics.processingNs = elapsedNs(scheduling::Clock::now() - processing);
                    metrics.physical = capture.counts();
                }
            }
            metrics.lockNs = elapsedNs(scheduling::Clock::now() - acquired);
            metrics.completedBlocks = completed;
            metrics.totalBlocks = totalBlocks;
            metrics.decision = controller.current();
            if (trace_) {
                try { metrics.load.outstanding = scheduling::outstanding(); } catch (...) {}
            }
            stamp(metrics);
            if (checkpointSaved) observe(RebuildPhase::CheckpointSaved);
            if (ending) return *ending;
            subsequent = true;
            std::this_thread::yield();
        }
    } catch (const std::exception& error) {
        if (!started) {
            throw;
        }
        try {
            StorageEngine engine;
            engine.initialize(root_);
            auto& arrays = engine.arrays();
            arrays.lockCatalog();
            auto job = reconcile(arrays, jobs_.load(id), false);
            if (job.status != RebuildStatus::Completed) {
                job.status = RebuildStatus::Failed;
                job.pauseRequested = false;
                job.lastError = error.what();
                jobs_.save(job);
            }
            return job;
        } catch (const std::exception& publicationError) {
            throw std::runtime_error(std::string(error.what()) + " (could not publish job result: " +
                                     publicationError.what() + ')');
        }
    }
}

}  // namespace raid
