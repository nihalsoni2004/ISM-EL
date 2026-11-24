#include "rebuild_test_support.h"

#include <iostream>

namespace {

using namespace rebuild_test;
using raid::RaidHealth;
using raid::RaidLevel;
using raid::RebuildManager;
using raid::RebuildStatus;
using test::rejects;

void rebuildOne(RaidLevel level, std::size_t count, std::size_t target) {
    const ArrayFixture fixture(level, count);
    const auto originals = fixture.snapshot();
    const auto metadata = readFile(fixture.metadata);
    failMember(fixture, target);
    addSpare(fixture, "disk90");
    RebuildManager manager(fixture.root.path(), 4);
    const auto job = manager.prepare("array0", fixture.ids[target], "disk90");
    const auto result = manager.run(job.id);
    if (result.status != RebuildStatus::Completed) {
        throw std::runtime_error("Rebuild did not complete: " + result.lastError);
    }
    require(result.completedBlocks == memberBlocks && verifyArray(fixture) == RaidHealth::Healthy,
            "replacement did not restore verified redundancy");
    require(readFile(sparePath(fixture, "disk90")) == originals[target], "rebuilt physical bytes differ");
    require(fixture.snapshot() == originals && readFile(fixture.metadata) == metadata, "original data changed");
    require(manager.listSpares().front().state == raid::DiskState::Online &&
            !manager.listSpares().front().available, "promoted spare remains available");
    {
        raid::StorageEngine engine;
        require(engine.initialize(fixture.root.path()), "reopen failed");
        const auto& array = engine.arrays().openArray("array0");
        require(array.configuration().members[target] == "disk90", "spare occupied the wrong physical position");
        rejects<std::invalid_argument>([&] { engine.arrays().requireUnownedDisk(fixture.ids[target]); },
                                       "retired member lost raw-write protection");
    }
}

void rebuildPairs() {
    for (std::size_t first = 0; first < 4; ++first) {
        for (auto second = first + 1; second < 4; ++second) {
            const ArrayFixture fixture(RaidLevel::Raid6, 4);
            const auto originals = fixture.snapshot();
            failMember(fixture, first);
            failMember(fixture, second);
            addSpare(fixture, "disk90");
            addSpare(fixture, "disk91");
            RebuildManager manager(fixture.root.path(), 4);
            const auto firstJob = manager.prepare("array0", fixture.ids[first], "disk90");
            const auto partial = manager.run(firstJob.id);
            if (partial.status != RebuildStatus::Completed) {
                throw std::runtime_error("First RAID 6 rebuild failed: " + partial.lastError);
            }
            require(verifyArray(fixture) == RaidHealth::Degraded, "first replacement prematurely reported healthy");

            const auto secondJob = manager.prepare("array0", fixture.ids[second], "disk91");
            const auto completed = manager.run(secondJob.id);
            if (completed.status != RebuildStatus::Completed) {
                throw std::runtime_error("Second RAID 6 rebuild failed: " + completed.lastError);
            }
            require(verifyArray(fixture) == RaidHealth::Healthy, "second replacement did not restore parity");
            require(readFile(sparePath(fixture, "disk90")) == originals[first] &&
                    readFile(sparePath(fixture, "disk91")) == originals[second], "RAID 6 physical bytes differ");
            require(fixture.snapshot() == originals, "original RAID 6 members changed");
        }
    }

    const ArrayFixture fixture(RaidLevel::Raid6, 4);
    failMember(fixture, 0);
    failMember(fixture, 1);
    failMember(fixture, 2);
    addSpare(fixture, "disk90");
    RebuildManager manager(fixture.root.path());
    rejects<std::invalid_argument>([&] { manager.prepare("array0", "disk0", "disk90"); },
                                   "rebuild accepted beyond RAID 6 tolerance");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc == 2 ? argv[1] : "";
        if (mode == "raid1") {
            for (const auto slot : {0U, 1U}) {
                rebuildOne(RaidLevel::Raid1, 2, slot);
            }
            const ArrayFixture fixture(RaidLevel::Raid0, 2);
            failMember(fixture, 0);
            addSpare(fixture, "disk90");
            RebuildManager manager(fixture.root.path());
            rejects<std::invalid_argument>([&] { manager.prepare("array0", "disk0", "disk90"); },
                                           "RAID 0 rebuild accepted");
        } else if (mode == "raid5") {
            for (const auto slot : {0U, 3U}) {
                rebuildOne(RaidLevel::Raid5, 4, slot);
            }
        } else if (mode == "raid6") {
            for (const auto slot : {0U, 2U, 3U}) {
                rebuildOne(RaidLevel::Raid6, 4, slot);
            }
            rebuildPairs();
        } else {
            throw std::invalid_argument("Unknown array rebuild test");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
