#pragma once

#include "failure_test_support.h"
#include "recovery/rebuild_manager.h"

namespace rebuild_test {
using namespace failure_test;

inline void addSpare(const ArrayFixture& fixture, std::string_view id, std::uint64_t blocks = memberBlocks) {
    raid::StorageEngine engine;
    require(engine.initialize(fixture.root.path()), "test engine initialization failed");
    engine.disks().createDisk(id, blocks * raid::raidBlockSize);
    engine.arrays().registerSpare(id);
    require(engine.shutdown(), "test engine shutdown failed");
}

inline void failMember(const ArrayFixture& fixture, std::size_t slot) {
    raid::StorageEngine engine;
    require(engine.initialize(fixture.root.path()), "test engine initialization failed");
    require(engine.arrays().failMember("array0", fixture.ids[slot]), "test failure was not applied");
}

inline fs::path sparePath(const ArrayFixture& fixture, std::string_view id) {
    return fixture.image(0).parent_path() / (std::string(id) + ".img");
}

inline fs::path jobPath(const ArrayFixture& fixture, const raid::RebuildJob& job) {
    return fixture.directory.parent_path() / "rebuild_jobs" / (job.id + ".job");
}

inline raid::RaidHealth verifyArray(const ArrayFixture& fixture) {
    raid::StorageEngine engine;
    require(engine.initialize(fixture.root.path()), "could not reopen rebuilt array");
    auto& array = engine.arrays().openArray("array0");
    fixture.expectRead(array);
    const auto health = array.status();
    if (health == raid::RaidHealth::Healthy) {
        array.verify();
    }
    return health;
}
}  // namespace rebuild_test
