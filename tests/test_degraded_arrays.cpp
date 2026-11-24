#include "failure_test_support.h"

#define NOMINMAX
#include <windows.h>

#include <array>
#include <iostream>

namespace {

using namespace failure_test;
using test::rejects;
using raid::RaidHealth;
using raid::RaidLevel;

void fail(const ArrayFixture& fixture, std::vector<std::size_t> members, RaidHealth expected) {
    raid::StorageEngine engine;
    require(engine.initialize(fixture.root.path()), "engine initialization failed");
    for (const auto index : members) {
        require(engine.arrays().failMember("array0", fixture.ids[index]), "state transition was not applied");
    }
    require(engine.arrays().getArrayStatus("array0").health == expected, "failure health did not match tolerance");
}

void expectAfterRestart(const ArrayFixture& fixture, RaidHealth expected) {
    raid::StorageEngine engine;
    require(engine.initialize(fixture.root.path()), "restart initialization failed");
    auto& array = engine.arrays().openArray("array0");
    require(array.status() == expected && engine.arrays().getArrayStatus("array0").health == expected,
            "reopened health disagrees with persisted availability");
    if (raid::readsPermitted(expected)) {
        fixture.expectRead(array);
    } else {
        std::array<std::uint8_t, raid::raidBlockSize> data{};
        rejects<std::runtime_error>([&] { array.readBlocks(0, 1, data.data(), data.size()); },
                                    "inaccessible array served data");
    }
    rejects<std::runtime_error>([&] { array.writeBlocks(0, 1, fixture.payload.data(), fixture.payload.size()); },
                                "unhealthy array accepted writes");
    rejects<std::runtime_error>([&] { array.verify(); }, "unhealthy array verified");
}

struct CloseFile {
    void operator()(void* handle) const noexcept { CloseHandle(handle); }
};

void testRaid0And1() {
    {
        const ArrayFixture fixture(RaidLevel::Raid0, 2);
        const auto before = fixture.snapshot();
        fail(fixture, {1}, RaidHealth::Failed);
        expectAfterRestart(fixture, RaidHealth::Failed);
        require(fixture.snapshot() == before, "RAID 0 failure touched data");
    }
    for (std::size_t member = 0; member < 2; ++member) {
        const ArrayFixture fixture(RaidLevel::Raid1, 2);
        const auto before = fixture.snapshot();
        fail(fixture, {member}, RaidHealth::Degraded);
        {
            // No read/write opens of the failed member can succeed during this test.
            const auto handle = CreateFileW(fixture.image(member).c_str(), GENERIC_READ, 0,
                                            nullptr, OPEN_EXISTING, 0, nullptr);
            require(handle != INVALID_HANDLE_VALUE, "could not lock out the failed mirror");
            const std::unique_ptr<void, CloseFile> exclusive(handle);
            expectAfterRestart(fixture, RaidHealth::Degraded);
        }
        fail(fixture, {1 - member}, RaidHealth::Failed);
        expectAfterRestart(fixture, RaidHealth::Failed);
        require(fixture.snapshot() == before, "RAID 1 failures touched data");
    }
}

void testRaid5() {
    for (const auto member : {std::size_t{0}, std::size_t{3}}) {
        const ArrayFixture fixture(RaidLevel::Raid5, 4);
        const auto before = fixture.snapshot();
        // Member 0 is data in row 0. Member 3 is parity in row 0 and data in later rows.
        fail(fixture, {member}, RaidHealth::Degraded);
        expectAfterRestart(fixture, RaidHealth::Degraded);
        fail(fixture, {(member + 1) % 4}, RaidHealth::Failed);
        expectAfterRestart(fixture, RaidHealth::Failed);
        require(fixture.snapshot() == before, "RAID 5 reconstruction wrote to a member");
    }
}

void testRaid6() {
    for (std::size_t first = 0; first < 4; ++first) {
        for (auto second = first + 1; second < 4; ++second) {
            const ArrayFixture fixture(RaidLevel::Raid6, 4);
            const auto before = fixture.snapshot();
            fail(fixture, {first}, RaidHealth::Degraded);
            expectAfterRestart(fixture, RaidHealth::Degraded);
            fail(fixture, {second}, RaidHealth::Degraded);
            // All logical blocks span every rotating role and require exact binary recovery.
            expectAfterRestart(fixture, RaidHealth::Degraded);
            const auto third = (second + 1) % 4 == first ? (second + 2) % 4 : (second + 1) % 4;
            fail(fixture, {third}, RaidHealth::Failed);
            expectAfterRestart(fixture, RaidHealth::Failed);
            require(fixture.snapshot() == before, "RAID 6 reconstruction wrote to a member");
        }
    }
}

void testInconsistency() {
    const ArrayFixture fixture(RaidLevel::Raid6, 4);
    const auto before = fixture.snapshot();
    const std::string marker = "schema_version=1\nparity_write=pending\n";
    writeFile(fixture.pending, marker);
    expectAfterRestart(fixture, RaidHealth::Inconsistent);
    for (std::size_t member = 0; member < 3; ++member) {
        fail(fixture, {member}, RaidHealth::Inconsistent);
        expectAfterRestart(fixture, RaidHealth::Inconsistent);
        require(readFile(fixture.pending) == marker, "failure injection cleared or rewrote the pending marker");
    }
    require(fixture.snapshot() == before, "inconsistent array was modified by failure management");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view name = argc == 2 ? argv[1] : "";
        if (name == "raid01") {
            testRaid0And1();
        } else if (name == "raid5") {
            testRaid5();
        } else if (name == "raid6") {
            testRaid6();
        } else if (name == "pending") {
            testInconsistency();
        } else {
            throw std::invalid_argument("Unknown persistent degraded array test");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
