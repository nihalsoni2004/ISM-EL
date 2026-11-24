#include "rebuild_test_support.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>

namespace {

using namespace rebuild_test;
using raid::RaidHealth;
using raid::RaidLevel;
using raid::RebuildManager;
using raid::RebuildPhase;
using raid::RebuildStatus;
using test::rejects;

struct CheckpointGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool reached = false;
    bool released = false;

    void observe(RebuildPhase phase) {
        if (phase != RebuildPhase::CheckpointSaved) {
            return;
        }
        std::unique_lock<std::mutex> lock(mutex);
        reached = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    }

    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        require(changed.wait_for(lock, std::chrono::seconds(10), [&] { return reached; }),
                "worker did not reach its first durable checkpoint");
    }

    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        changed.notify_all();
    }
};

struct ReleaseGate {
    CheckpointGate& gate;
    ~ReleaseGate() { gate.release(); }
};

void testConcurrency(bool failSource) {
    const ArrayFixture fixture(RaidLevel::Raid1, 2);
    failMember(fixture, 0);
    addSpare(fixture, "disk90");
    CheckpointGate gate;
    RebuildManager worker(fixture.root.path(), 4, [&](auto phase) { gate.observe(phase); });
    const auto job = worker.prepare("array0", "disk0", "disk90");
    auto execution = std::async(std::launch::async, [&] { return worker.run(job.id); });
    const ReleaseGate cleanup{gate};
    gate.wait();

    RebuildManager foreground(fixture.root.path(), 4);
    for (int check = 0; check < 3; ++check) {
        const auto active = foreground.status(job.id);
        require(active.status == RebuildStatus::Running && active.completedBlocks == 4,
                "status did not observe live worker and durable progress");
    }
    require(verifyArray(fixture) == RaidHealth::Degraded, "foreground read saw unfinished spare as online");
    {
        raid::StorageEngine engine;
        require(engine.initialize(fixture.root.path()), "test engine initialization failed");
        auto& array = engine.arrays().openArray("array0");
        rejects<std::runtime_error>([&] {
            array.writeBlocks(0, 1, fixture.payload.data(), fixture.payload.size());
        }, "foreground write succeeded during rebuilding");
    }
    rejects<std::runtime_error>([&] { foreground.run(job.id); }, "duplicate worker acquired job");

    if (failSource) {
        failMember(fixture, 1);
    } else {
        auto one = std::async(std::launch::async, [&] { return foreground.pause(job.id); });
        auto two = std::async(std::launch::async, [&] { return foreground.pause(job.id); });
        require(one.get().pauseRequested && two.get().pauseRequested, "concurrent pause requests were lost");
    }
    gate.release();
    const auto result = execution.get();
    require(result.completedBlocks == 4, "worker advanced beyond signaled checkpoint");
    if (failSource) {
        require(result.status == RebuildStatus::Failed && !result.lastError.empty(),
                "source failure did not stop reconstruction");
        return;
    }
    require(result.status == RebuildStatus::Paused, "worker missed persisted pause");
    require(foreground.run(job.id).status == RebuildStatus::Completed && verifyArray(fixture) == RaidHealth::Healthy,
            "worker did not resume after cross-manager pause");
}

void testInterruption() {
    const ArrayFixture fixture(RaidLevel::Raid1, 2);
    const auto original = readFile(fixture.image(0));
    failMember(fixture, 0);
    addSpare(fixture, "disk90");
    RebuildManager manager(fixture.root.path(), 4);
    const auto job = manager.prepare("array0", "disk0", "disk90");
    const auto paused = manager.run(job.id, 1);
    require(paused.status == RebuildStatus::Paused && paused.completedBlocks == 4, "no real checkpoint was written");
    const auto path = jobPath(fixture, job);
    auto text = readFile(path);
    text.replace(text.find("status=PAUSED"), std::string_view("status=PAUSED").size(), "status=RUNNING");
    writeFile(path, text);  // Simulate process death after a durable checkpoint, with no worker lease.
    RebuildManager restarted(fixture.root.path(), 4);
    require(restarted.status(job.id).status == RebuildStatus::Interrupted, "orphaned RUNNING job was not detected");
    require(restarted.run(job.id).status == RebuildStatus::Completed && verifyArray(fixture) == RaidHealth::Healthy &&
            readFile(sparePath(fixture, "disk90")) == original, "interrupted job failed to resume");
    text = readFile(path);
    text.replace(text.find("status=COMPLETED"), std::string_view("status=COMPLETED").size(), "status=RUNNING");
    writeFile(path, text);  // Simulate death after committing membership but before updating the job.
    require(restarted.status(job.id).status == RebuildStatus::Completed &&
            readFile(path).find("status=COMPLETED") != std::string::npos, "commit reconciliation failed");
}

void testUnsafeResume(bool replace) {
    const ArrayFixture fixture(RaidLevel::Raid1, 2);
    failMember(fixture, 0);
    addSpare(fixture, "disk90");
    RebuildManager manager(fixture.root.path(), 4);
    const auto job = manager.prepare("array0", "disk0", "disk90");
    require(manager.run(job.id, 1).completedBlocks == 4, "pause checkpoint missing");
    const auto path = sparePath(fixture, "disk90");
    {
        raid::VirtualDisk image;
        if (replace) {
            fs::rename(path, fixture.root.path() / "saved-spare.img");
            image.create(path, memberBlocks * raid::raidBlockSize);
        } else {
            image.open(path);
            std::vector<std::uint8_t> zero(raid::raidBlockSize);
            image.writeBlocks(0, 1, zero.data(), zero.size());
        }
        image.close();
    }
    const auto result = manager.run(job.id);
    require(result.status == RebuildStatus::Failed &&
            result.lastError.find(replace ? "identity" : "verification failed") != std::string::npos,
            "changed replacement passed validation");
    require(verifyArray(fixture) == RaidHealth::Degraded, "failed replacement was promoted");
}

void testFaults() {
    for (const auto fault : {RebuildPhase::Start, RebuildPhase::SourceRead, RebuildPhase::SpareWrite,
                             RebuildPhase::SpareFlush, RebuildPhase::PublishCheckpoint,
                             RebuildPhase::Verify, RebuildPhase::Promote, RebuildPhase::PromotionCommitted}) {
        const ArrayFixture fixture(RaidLevel::Raid1, 2);
        const auto originals = fixture.snapshot();
        failMember(fixture, 0);
        addSpare(fixture, "disk90");
        RebuildManager manager(fixture.root.path(), 4, [fault](auto phase) {
            if (phase == fault) {
                throw std::runtime_error("injected rebuild boundary error");
            }
        });
        const auto job = manager.prepare("array0", "disk0", "disk90");
        if (fault == RebuildPhase::Start) {
            rejects<std::runtime_error>([&] { manager.run(job.id); }, "start publication failure was ignored");
            require(manager.status(job.id).status == RebuildStatus::Prepared, "unpublished start changed job state");
        } else {
            const auto result = manager.run(job.id);
            if (fault == RebuildPhase::PromotionCommitted) {
                require(result.status == RebuildStatus::Completed && verifyArray(fixture) == RaidHealth::Healthy,
                        "committed membership was not reconciled");
            } else {
                require(result.status == RebuildStatus::Failed && result.lastError == "injected rebuild boundary error" &&
                        verifyArray(fixture) == RaidHealth::Degraded, "failed boundary promoted a spare");
                const auto expectedBlocks = fault == RebuildPhase::Verify || fault == RebuildPhase::Promote
                    ? memberBlocks : 0;
                require(result.completedBlocks == expectedBlocks, "failed batch advanced the durable checkpoint");
            }
        }
        require(fixture.snapshot() == originals, "failure modified an original member");
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc == 2 ? argv[1] : "";
        if (mode == "concurrent") {
            testConcurrency(false);
            testConcurrency(true);
        } else if (mode == "restart") {
            testInterruption();
            testUnsafeResume(true);
            testUnsafeResume(false);
        } else if (mode == "faults") {
            testFaults();
        } else {
            throw std::invalid_argument("Unknown rebuild persistence test");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
