#include "failure_test_support.h"

#define NOMINMAX
#include <windows.h>

#include <array>
#include <iostream>

namespace {

using namespace failure_test;
using test::rejects;
using raid::DiskState;
using raid::FailureCause;
using raid::RaidHealth;
using raid::RaidLevel;

struct CloseFile {
    void operator()(void* handle) const noexcept { CloseHandle(handle); }
};

void testStateTransitions() {
    const ArrayFixture fixture(RaidLevel::Raid1, 2);
    const auto images = fixture.snapshot();
    const auto staticMetadata = readFile(fixture.metadata);
    {
        raid::StorageEngine engine;
        require(engine.initialize(fixture.root.path()), "initialization failed");
        auto& arrays = engine.arrays();
        auto& array = arrays.openArray("array0");
        auto report = arrays.getArrayStatus("array0");
        require(report.health == RaidHealth::Healthy && !fs::exists(fixture.state), "unexpected initial state");
        require(report.members[0].failure.state == DiskState::Online, "default not ONLINE");
        rejects<std::invalid_argument>([&] { arrays.failMember("array0", "../disk0"); }, "unsafe member accepted");
        rejects<std::invalid_argument>([&] { arrays.failMember("../array0", "disk0"); }, "unsafe array accepted");
        rejects<std::invalid_argument>([&] { arrays.failMember("array0", "disk9"); }, "unowned disk accepted");
        require(!fs::exists(fixture.state), "invalid failure created metadata");

        require(arrays.failMember("array0", "disk0"), "ONLINE -> FAILED was not recorded");
        const auto published = readFile(fixture.state);
        require(published.find("member_states=FAILED,ONLINE") != std::string::npos &&
                published.find("failure_causes=INJECTED,NONE") != std::string::npos, "wrong persisted state");
        require(!arrays.failMember("array0", "disk0") && readFile(fixture.state) == published, "duplicate not idempotent");
        require(array.status() == RaidHealth::Degraded && !array.memberAvailable(0),
                "live controller did not receive the transition");
        fixture.expectRead(array);
        {
            const auto handle = CreateFileW(fixture.state.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                            nullptr, OPEN_EXISTING, 0, nullptr);
            require(handle != INVALID_HANDLE_VALUE, "could not lock the old state record");
            const std::unique_ptr<void, CloseFile> lock(handle);
            rejects<fs::filesystem_error>([&] { arrays.failMember("array0", "disk1"); },
                                          "unexpected successful replacement of locked state");
            require(readFile(fixture.state) == published && array.memberAvailable(1),
                    "failed publication changed the original state or live controller");
            for (const auto& entry : fs::directory_iterator(fixture.directory)) {
                require(entry.path().extension() != ".tmp", "failed publication left a staging file");
            }
        }
        require(engine.shutdown(), "test shutdown failed");
    }
    {
        raid::StorageEngine restarted;
        require(restarted.initialize(fixture.root.path()), "restart failed");
        auto report = restarted.arrays().getArrayStatus("array0");
        require(report.health == RaidHealth::Degraded && report.members[0].failure.cause == FailureCause::Injected &&
                report.members[0].imagePresent && !report.members[0].available, "failed state lost on restart");
        require(restarted.arrays().failMember("array0", "disk1"), "second failure was not recorded");
        require(restarted.arrays().getArrayStatus("array0").health == RaidHealth::Failed, "second failure not reflected");
        require(readFile(fixture.state).find("member_states=FAILED,FAILED") != std::string::npos,
                "replacement lost previous failed member");
        for (const auto& entry : fs::directory_iterator(fixture.directory)) {
            require(entry.path().extension() != ".tmp", "successful publication left a staging file");
        }
    }
    require(fixture.snapshot() == images && readFile(fixture.metadata) == staticMetadata,
            "failure injection mutated image or configuration bytes");
}

void testInvalidMetadata() {
    const ArrayFixture fixture(RaidLevel::Raid1, 2);
    const std::string valid = "schema_version=1\narray_id=array0\nmembers=disk0,disk1\n"
                              "member_states=FAILED,ONLINE\nfailure_causes=INJECTED,NONE\n";
    raid::StorageEngine engine;
    require(engine.initialize(fixture.root.path()), "initialization failed");
    const std::array<std::pair<std::string_view, std::string_view>, 7> mutations{{
        {"schema_version=1", "schema_version=5"}, {"array_id=array0", "array_id=array1"},
        {"members=disk0,disk1", "members=disk1,disk0"},
        {"member_states=FAILED,ONLINE", "member_states=FAILED"},
        {"member_states=FAILED,ONLINE", "member_states=FAILED,SPARE"},
        {"member_states=FAILED,ONLINE", "member_states=REBUILDING,ONLINE"},
        {"failure_causes=INJECTED,NONE", "failure_causes=NONE,NONE"}}};
    for (const auto& [from, to] : mutations) {
        auto malformed = valid;
        malformed.replace(malformed.find(from), from.size(), to);
        writeFile(fixture.state, malformed);
        rejects<std::invalid_argument>([&] { engine.arrays().openArray("array0"); }, "invalid state opened");
        rejects<std::invalid_argument>([&] { engine.arrays().failMember("array0", "disk1"); }, "invalid state reset");
        require(readFile(fixture.state) == malformed, "invalid state was overwritten");
    }
    writeFile(fixture.state, "junk\n");
    rejects<std::invalid_argument>([&] { engine.arrays().getArrayStatus("array0"); }, "malformed record accepted");
    fs::remove(fixture.state);
    const auto external = fixture.root.path() / "outside.state";
    writeFile(external, valid);
    fs::create_hard_link(external, fixture.state);
    rejects<std::invalid_argument>([&] { engine.arrays().failMember("array0", "disk1"); }, "linked state accepted");
    require(readFile(external) == valid, "linked state target changed");
}

void testMissingImages() {
    const std::array<std::pair<RaidLevel, std::size_t>, 4> configurations{{
        {RaidLevel::Raid0, 2}, {RaidLevel::Raid1, 2}, {RaidLevel::Raid5, 4}, {RaidLevel::Raid6, 4}}};
    for (const auto& [level, count] : configurations) {
        const ArrayFixture fixture(level, count);
        const auto original = fixture.snapshot();
        const auto saved = fixture.root.path() / "saved.img";
        fs::rename(fixture.image(0), saved);
        const auto expected = level == RaidLevel::Raid0 ? RaidHealth::Failed : RaidHealth::Degraded;
        {
            raid::StorageEngine engine;
            require(engine.initialize(fixture.root.path()), "initialization failed");
            auto& array = engine.arrays().openArray("array0");
            const auto report = engine.arrays().getArrayStatus("array0");
            require(report.health == expected && array.status() == expected, "wrong missing-image health");
            require(report.members[0].failure.cause == FailureCause::MissingImage && !report.members[0].imagePresent,
                    "missing image not diagnosed");
            if (expected == RaidHealth::Degraded) {
                fixture.expectRead(array);
            }
            rejects<std::invalid_argument>([&] { engine.arrays().requireUnownedDisk("disk0"); }, "missing disk lost ownership");
            require(!fs::exists(fixture.image(0)), "reopened array created a replacement");
        }
        fs::rename(saved, fixture.image(0));
        {
            raid::StorageEngine restarted;
            require(restarted.initialize(fixture.root.path()), "restart failed");
            const auto report = restarted.arrays().getArrayStatus("array0");
            require(report.health == expected && report.members[0].imagePresent && !report.members[0].available,
                    "reappeared image was silently trusted");
            require(report.members[0].failure.cause == FailureCause::MissingImage, "first failure cause lost");
        }
        require(fixture.snapshot() == original, "missing/reappearing handling altered member bytes");
    }
}

void testReplacementAndLegacy() {
    {
        const ArrayFixture fixture(RaidLevel::Raid1, 2);
        fs::rename(fixture.image(0), fixture.root.path() / "old.img");
        {
            raid::VirtualDisk replacement;
            replacement.create(fixture.image(0), memberBlocks * raid::raidBlockSize);
            replacement.close();
        }
        raid::StorageEngine engine;
        require(engine.initialize(fixture.root.path()), "initialization failed");
        auto& array = engine.arrays().openArray("array0");
        fixture.expectRead(array);
        const auto status = engine.arrays().getArrayStatus("array0");
        require(status.members[0].failure.cause == FailureCause::ImageChanged && status.members[0].imagePresent,
                "unobserved same-path replacement was trusted");
    }
    {
        const ArrayFixture fixture(RaidLevel::Raid1, 2);
        auto legacy = readFile(fixture.metadata);
        legacy.resize(legacy.find("member_identities="));
        writeFile(fixture.metadata, legacy);
        {
            raid::StorageEngine engine;
            require(engine.initialize(fixture.root.path()), "initialization failed");
            fixture.expectRead(engine.arrays().openArray("array0"));
            require(!fs::exists(fixture.state) && readFile(fixture.metadata) == legacy, "legacy open rewrote metadata");
        }
        const auto saved = fixture.root.path() / "saved.img";
        fs::rename(fixture.image(0), saved);
        fs::create_hard_link(saved, fixture.image(0));
        {
            raid::StorageEngine engine;
            require(engine.initialize(fixture.root.path()), "initialization failed");
            rejects<std::runtime_error>([&] { engine.arrays().openArray("array0"); }, "linked member opened");
            require(!fs::exists(fixture.state), "unsafe link was misreported as a missing image");
        }
        fs::remove(fixture.image(0));
        {
            raid::StorageEngine engine;
            require(engine.initialize(fixture.root.path()), "initialization failed");
            fixture.expectRead(engine.arrays().openArray("array0"));
        }
        fs::rename(saved, fixture.image(0));
        raid::StorageEngine restarted;
        require(restarted.initialize(fixture.root.path()), "restart failed");
        require(restarted.arrays().getArrayStatus("array0").members[0].failure.cause == FailureCause::MissingImage,
                "legacy reappearance cleared missing state");
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view test = argc == 2 ? argv[1] : "";
        if (test == "transitions") {
            testStateTransitions();
        } else if (test == "metadata") {
            testInvalidMetadata();
        } else if (test == "missing") {
            testMissingImages();
        } else if (test == "replacement") {
            testReplacementAndLegacy();
        } else {
            throw std::invalid_argument("Unknown failure manager test");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
