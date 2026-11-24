#include "core/engine.h"
#include "provisioning/provisioning_manager.h"
#include "recovery/rebuild_manager.h"
#include "test_support.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <thread>

namespace {

using namespace raid;
using test::rejects;
using test::require;
namespace fs = std::filesystem;
using Buffer = std::vector<std::uint8_t>;

void createArray(const fs::path& root, RaidLevel level, std::uint64_t blocks) {
    StorageEngine engine;
    engine.initialize(root);
    std::vector<std::string> members;
    const auto count = level == RaidLevel::Raid0 || level == RaidLevel::Raid1 ? 2 : 4;
    for (int index = 0; index < count; ++index) {
        members.push_back("disk" + std::to_string(index));
        engine.disks().createDisk(members.back(), blocks * raidBlockSize);
    }
    engine.arrays().createArray("array0", level, std::move(members),
                                level == RaidLevel::Raid1 ? mirrorStripeUnitBlocks : defaultStripeUnitBlocks);
}

struct Fixture {
    test::TempDirectory root;
    ProvisioningManager manager{root.path()};

    explicit Fixture(RaidLevel level = RaidLevel::Raid1, std::uint64_t blocks = 160) {
        createArray(root.path(), level, blocks);
    }

    fs::path record() const { return root.path() / "data/metadata/provisioning/array0.pool"; }
    void provision(std::uint64_t blocks = 385) {
        manager.createPool("pool0", "array0");
        manager.createLun("lun0", "pool0", blocks * raidBlockSize);
    }
};

Buffer binary(std::size_t blocks, unsigned seed) {
    std::mt19937 random(seed);
    Buffer data(blocks * raidBlockSize);
    for (auto& byte : data) {
        byte = static_cast<std::uint8_t>(random());
    }
    data.front() = 0;
    return data;
}

Buffer read(ProvisioningManager& manager, std::string_view id, std::uint64_t lba, std::size_t count = 1) {
    Buffer data(count * raidBlockSize, 0xff);
    manager.readBlocks(id, lba, count, data.data(), data.size());
    return data;
}

LunWriteResult write(ProvisioningManager& manager, std::string_view id, std::uint64_t lba, const Buffer& data) {
    return manager.writeBlocks(id, lba, data.size() / raidBlockSize, data.data(), data.size());
}

void testData() {
    Fixture fixture;
    auto& manager = fixture.manager;
    const Buffer zero(raidBlockSize);
    const auto stale = binary(16, 104);
    {
        StorageEngine engine;
        engine.initialize(fixture.root.path());
        engine.arrays().openArray("array0").writeBlocks(0, 16, stale.data(), stale.size());
    }
    fixture.provision(81);
    manager.createLun("lun1", "pool0", 128 * raidBlockSize);
    require(manager.poolInfo("pool0").allocatedChunks == 0, "LUN creation allocated capacity");
    require(read(manager, "lun0", 0, 18) == Buffer(18 * raidBlockSize) &&
            manager.lunInfo("lun0").allocatedChunks == 0 && !manager.map("lun0", 0).physicalChunk,
            "unmapped read allocated or exposed data");
    {
        StorageEngine engine;
        engine.initialize(fixture.root.path());
        auto& array = engine.arrays().openArray("array0");
        Buffer actual(stale.size());
        array.readBlocks(0, 16, actual.data(), actual.size());
        require(actual == stale, "pool creation rewrote pre-existing raw data");
        rejects<std::invalid_argument>([&] { array.writeBlocks(0, 16, stale.data(), stale.size()); },
                                      "ordinary managed controller bypassed pool ownership");
    }

    const auto a = binary(1, 41);
    const auto b = binary(1, 52);
    require(write(manager, "lun0", 10, a).newChunks == 1 &&
            write(manager, "lun0", 11, b).newChunks == 0, "same chunk was allocated twice");
    require(read(manager, "lun0", 9) == zero && read(manager, "lun0", 10) == a &&
            read(manager, "lun0", 11) == b && read(manager, "lun0", 12) == zero,
            "chunk initialization exposed stale bytes");

    const auto cross = binary(3, 117);
    require(write(manager, "lun0", 15, cross).newChunks == 1 && read(manager, "lun0", 15, 3) == cross,
            "cross-chunk binary write failed");
    auto expected = Buffer(18 * raidBlockSize);
    std::copy_n(cross.data() + 2 * raidBlockSize, raidBlockSize, expected.data());
    require(read(manager, "lun0", 17, 18) == expected, "mixed mapped and unmapped read had wrong order");
    write(manager, "lun0", 0, a);
    write(manager, "lun1", 0, b);
    write(manager, "lun0", 80, b);
    write(manager, "lun0", 10, b);
    require(manager.map("lun0", 0).physicalChunk == 0 && manager.map("lun1", 0).physicalChunk == 2 &&
            manager.map("lun0", 17).raidLba == 17 && manager.map("lun0", 80).raidLba == 48,
            "unexpected logical-to-physical mapping");
    require(manager.verifyPool("pool0").allocatedChunks == 4, "wrong total allocation");
    ProvisioningManager reopened(fixture.root.path());
    require(read(reopened, "lun0", 0) == a && read(reopened, "lun0", 10) == b &&
            read(reopened, "lun0", 80) == b && read(reopened, "lun1", 0) == b &&
            read(reopened, "lun1", 10) == zero, "reopened volumes lost data or isolation");

    rejects<std::invalid_argument>([&] { read(reopened, "lun0", 80, 2); }, "invalid logical LBA");
    rejects<std::invalid_argument>([&] { reopened.map("lun0", UINT64_MAX); }, "overflowing logical LBA");
    rejects<std::invalid_argument>([&] { reopened.createLun("lun1", "pool0", 4096); }, "duplicate LUN");
    rejects<std::invalid_argument>([&] { reopened.createLun("lun2", "pool9", 4096); }, "unknown pool");
    rejects<std::invalid_argument>([&] { reopened.createLun("../lun2", "pool0", 4096); }, "path-like LUN");
    rejects<std::invalid_argument>([&] { reopened.createLun("lun2", "pool0", 4097); }, "unaligned LUN");
    rejects<std::invalid_argument>([&] { reopened.readBlocks("lun0", 0, 1, nullptr, 4096); }, "null buffer");
    rejects<std::invalid_argument>([&] { reopened.writeBlocks("lun0", 0, UINT64_MAX, a.data(), a.size()); },
                                   "overflowing transfer");
}

void testCapacity() {
    Fixture fixture;
    auto& manager = fixture.manager;
    rejects<std::invalid_argument>([&] { manager.createPool("pool0", "array0", {0}); }, "zero chunk size");
    rejects<std::invalid_argument>([&] { manager.createPool("pool0", "array0", {161}); }, "no complete chunk");
    rejects<std::invalid_argument>([&] { manager.createPool("pool0", "array0", {16, 75, 50, 0}); },
                                   "reversed watermarks");
    const auto pool = manager.createPool("pool0", "array0", {16, 70, 90, 3});
    require(pool.totalChunks == 10 && pool.freeChunks == 10 && pool.allocatedChunks == 0, "bad empty pool");
    rejects<std::invalid_argument>([&] { manager.createPool("pool2", "array0"); }, "duplicate RAID ownership");
    manager.createLun("lun0", "pool0", 256 * raidBlockSize);
    manager.createLun("lun1", "pool0", 224 * raidBlockSize);
    rejects<std::invalid_argument>([&] { manager.createLun("lun2", "pool0", 4096); }, "overprovisioning limit");
    require(manager.poolInfo("pool0").provisionedBlocks == 480, "bad aggregate provisioned capacity");
    require(manager.setWatermarks("pool0", 60, 80).options.warning == 60, "watermarks not set");
    manager.setWatermarks("pool0", 70, 90);
    const auto old = binary(1, 112);
    for (std::uint64_t chunk = 0; chunk < 9; ++chunk) {
        write(manager, "lun0", chunk * 16 + 15, old);
        const auto expected = chunk == 8 ? CapacityState::Critical :
                              chunk >= 6 ? CapacityState::Warning : CapacityState::Normal;
        require(manager.poolInfo("pool0").capacityState == expected, "wrong capacity watermark");
    }
    // Includes a mapped block and two new chunks with only one physical chunk free.
    rejects<std::runtime_error>([&] { write(manager, "lun0", 143, binary(18, 19)); },
                                "out-of-space preflight permitted a partial write");
    require(read(manager, "lun0", 143) == old && !manager.map("lun0", 144).physicalChunk &&
            manager.poolInfo("pool0").reservedChunks == 0, "out-of-space changed data or metadata");
    write(manager, "lun1", 0, old);
    auto reopened = ProvisioningManager(fixture.root.path());
    require(reopened.poolInfo("pool0").capacityState == CapacityState::Full &&
            reopened.poolInfo("pool0").freeChunks == 0, "full pool did not survive restart");
    rejects<std::runtime_error>([&] { write(reopened, "lun0", 160, old); }, "full pool accepted new chunk");
    const auto update = binary(1, 88);
    require(write(reopened, "lun0", 143, update).newChunks == 0 && read(reopened, "lun1", 0) == old &&
            read(reopened, "lun0", 143) == update, "full pool blocked a mapped write or corrupted another LUN");

    Fixture unbounded;
    unbounded.manager.createPool("pool0", "array0", {17});
    require(unbounded.manager.poolInfo("pool0").totalChunks == 9, "partial physical chunk was exposed");
    unbounded.manager.createLun("lun0", "pool0", UINT64_MAX - UINT64_MAX % raidBlockSize);
    rejects<std::overflow_error>([&] { unbounded.manager.createLun("lun1", "pool0", 4096); },
                                 "aggregate provisioned capacity overflow");
}

std::string readText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot read fixture metadata");
    return {std::istreambuf_iterator<char>(input), {}};
}

void writeText(const fs::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    require(output.good(), "cannot write fixture metadata");
}

std::string changed(std::string text, std::string_view from, std::string_view to) {
    const auto start = text.find(from);
    require(start != std::string::npos, "missing test metadata field");
    return text.replace(start, from.size(), to);
}

void testMetadata() {
    Fixture fixture;
    fixture.provision();
    fixture.manager.createLun("lun1", "pool0", 4096);
    write(fixture.manager, "lun0", 0, binary(1, 93));
    write(fixture.manager, "lun1", 0, binary(1, 94));
    const auto path = fixture.record();
    const auto original = readText(path);
    for (const auto& corrupt : {
             changed(original, "schema_version=1", "schema_version=2"),
             changed(original, "mapping=lun1,0,1", "mapping=lun1,0,0"),
             changed(original, "mapping=lun1,0,1", "mapping=lun1,0,10"),
             changed(original, "mapping=lun1,0,1", "mapping=lun1,1,1"),
             changed(original, "mapping=lun1,0,1", "mapping=lun9,0,1"),
             changed(original, "mapping=lun1,0,1", "mapping=lun0,0,1"),
             changed(original, "free_chunks=8", "free_chunks=9"),
             changed(original, "reserved_chunks=0", "reserved_chunks=1"),
             changed(original, "end=1\n", "")}) {
        writeText(path, corrupt);
        rejects<std::invalid_argument>([&] { fixture.manager.verifyPool("pool0"); }, "accepted corrupt record");
    }
    writeText(path, original);
    require(fixture.manager.verifyPool("pool0").allocatedChunks == 2, "valid metadata did not reopen");

    const auto array = fixture.root.path() / "data/metadata/raid_arrays/array0.raid";
    const auto saved = array.parent_path() / "original.raid.saved";
    fs::rename(array, saved);
    fs::copy_file(saved, array);
    rejects<std::runtime_error>([&] { fixture.manager.verifyPool("pool0"); }, "backing identity substitution");
}

void testLegacy() {
    Fixture fixture;
    const auto path = fixture.root.path() / "data/metadata/raid_arrays/array0.raid";
    auto metadata = readText(path);
    metadata.erase(metadata.find("member_identities="));
    writeText(path, metadata);
    fixture.provision();
    const auto expected = binary(1, 87);
    write(fixture.manager, "lun0", 0, expected);
    require(read(fixture.manager, "lun0", 0) == expected, "legacy pool failed to reopen");
    const auto image = fixture.root.path() / "data/virtual_disks/disk0.img";
    const auto original = image.parent_path() / "saved.img";
    fs::rename(image, original);
    fs::copy_file(original, image);
    rejects<std::runtime_error>([&] { read(fixture.manager, "lun0", 0); }, "legacy image substitution");
}

void testFaults() {
    for (const auto failAt : {AllocationPhase::BeforeReservation, AllocationPhase::Reserved,
                              AllocationPhase::BeforeChunkWrite, AllocationPhase::ChunkWritten,
                              AllocationPhase::BeforeCommit, AllocationPhase::Committed}) {
        Fixture fixture;
        fixture.provision();
        fixture.manager.createLun("lun1", "pool0", 32 * raidBlockSize);
        ProvisioningManager failing(fixture.root.path(), [failAt](auto phase) {
            if (phase == failAt) {
                throw std::runtime_error("injected interruption");
            }
        });
        const auto data = binary(2, 178);
        rejects<std::runtime_error>([&] { write(failing, "lun0", 15, data); }, "ignored interruption");
        ProvisioningManager reopened(fixture.root.path());
        const auto snapshot = reopened.verifyPool("pool0");
        if (failAt == AllocationPhase::Committed) {
            require(snapshot.allocatedChunks == 2 && snapshot.reservedChunks == 0 &&
                    read(reopened, "lun0", 15, 2) == data, "durable publication did not survive interruption");
        } else {
            const auto reserved = failAt == AllocationPhase::BeforeReservation ? 0U : 2U;
            require(snapshot.allocatedChunks == 0 && snapshot.reservedChunks == reserved &&
                    snapshot.freeChunks == snapshot.totalChunks - reserved, "incorrect orphan accounting");
            require(read(reopened, "lun0", 15, 2) == Buffer(2 * raidBlockSize), "orphaned data was visible");
            const auto replacement = binary(1, 55);
            write(reopened, "lun1", 0, replacement);
            require(reopened.map("lun1", 0).physicalChunk == 0 && reopened.poolInfo("pool0").reservedChunks == 0 &&
                    read(reopened, "lun1", 0) == replacement && read(reopened, "lun1", 15) == Buffer(raidBlockSize),
                    "a different LUN did not safely reclaim and zero the orphan");
            write(reopened, "lun0", 15, data);
            require(reopened.map("lun0", 15).physicalChunk == 1 && reopened.map("lun0", 16).physicalChunk == 2 &&
                    read(reopened, "lun0", 15, 2) == data, "retry conflicted with another LUN");
        }
    }
}

void testPublicationFailure() {
    Fixture fixture;
    fixture.provision();
    HANDLE denial = INVALID_HANDLE_VALUE;
    ProvisioningManager manager(fixture.root.path(), [&](auto phase) {
        if (phase == AllocationPhase::BeforeCommit) {
            denial = CreateFileW(fixture.record().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            require(denial != INVALID_HANDLE_VALUE, "could not set up metadata sharing denial");
        }
    });
    const auto expected = binary(1, 55);
    std::string error;
    try {
        write(manager, "lun0", 3, expected);
    } catch (const std::exception& failure) {
        error = failure.what();
    }
    const bool injected = denial != INVALID_HANDLE_VALUE;
    if (injected) {
        CloseHandle(denial);
    }
    require(injected && error.find("Replace storage metadata") != std::string::npos,
            "metadata rename rejection did not occur");
    require(fixture.manager.verifyPool("pool0").reservedChunks == 1 &&
            read(fixture.manager, "lun0", 3) == Buffer(raidBlockSize), "failed metadata publication exposed data");
    write(fixture.manager, "lun0", 3, expected);
    require(fixture.manager.verifyPool("pool0").reservedChunks == 0 &&
            read(fixture.manager, "lun0", 3) == expected, "retry did not recover publication failure");
}

void testTransientPublicationDenial() {
    Fixture fixture;
    fixture.provision();
    std::future<void> release;
    ProvisioningManager manager(fixture.root.path(), [&](auto phase) {
        if (phase == AllocationPhase::BeforeCommit) {
            const auto handle = CreateFileW(fixture.record().c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            require(handle != INVALID_HANDLE_VALUE, "could not open temporary metadata reader");
            release = std::async(std::launch::async, [handle] {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                CloseHandle(handle);
            });
        }
    });
    const auto data = binary(1, 33);
    require(write(manager, "lun0", 0, data).newChunks == 1 && read(fixture.manager, "lun0", 0) == data,
            "short-lived metadata sharing denial prevented publication");
    release.get();
}

void testConcurrent() {
    Fixture fixture;
    fixture.provision();
    fixture.manager.createLun("lun1", "pool0", 32 * raidBlockSize);
    std::promise<void> reserved;
    std::promise<void> continueWrite;
    const auto resume = continueWrite.get_future();
    const auto firstData = binary(1, 14);
    const auto secondData = binary(1, 37);
    ProvisioningManager first(fixture.root.path(), [&](auto phase) {
        if (phase == AllocationPhase::Reserved) {
            reserved.set_value();
            resume.wait();
        }
    });
    auto firstWriter = std::async(std::launch::async, [&] { return write(first, "lun0", 0, firstData); });
    auto observed = reserved.get_future();
    if (observed.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        continueWrite.set_value();
        firstWriter.get();
        throw std::runtime_error("first writer did not reserve");
    }
    std::promise<void> entered;
    auto secondWriter = std::async(std::launch::async, [&] {
        entered.set_value();
        return write(fixture.manager, "lun1", 0, secondData);
    });
    entered.get_future().wait();
    const auto blocked = secondWriter.wait_for(std::chrono::milliseconds(150)) == std::future_status::timeout;
    continueWrite.set_value();
    require(firstWriter.get().firstPhysicalChunk == 0 && secondWriter.get().firstPhysicalChunk == 1 &&
            blocked && read(fixture.manager, "lun0", 0) == firstData &&
            read(fixture.manager, "lun1", 0) == secondData, "concurrent writers did not serialize safely");
}

void testIntegration(RaidLevel level) {
    Fixture fixture(level, 64);
    fixture.provision();
    const auto data = binary(4, 224);
    auto& manager = fixture.manager;
    write(manager, "lun0", 14, data);
    require(read(manager, "lun0", 14, 4) == data, "persisted array-backed data differs");
    {
        StorageEngine engine;
        engine.initialize(fixture.root.path());
        auto& array = engine.arrays().openArray("array0");
        array.verify();
        // Read through the physical member mappings to confirm actual striping and mirroring.
        for (std::size_t index = 0; index < 4; ++index) {
            for (const auto location : array.map(14 + index)) {
                if (level == RaidLevel::Raid5 || level == RaidLevel::Raid6) {
                    break;  // The full verification above exercises the rotating parity.
                }
                Buffer member(raidBlockSize);
                engine.disks().openDisk(array.configuration().members[location.memberIndex]).readBlocks(
                    location.physicalLba, 1, member.data(), member.size());
                require(std::equal(member.begin(), member.end(), data.data() + index * raidBlockSize),
                        "incorrect member data");
            }
        }
        engine.arrays().failMember("array0", "disk0");
    }
    const auto expectedHealth = level == RaidLevel::Raid0 ? RaidHealth::Failed : RaidHealth::Degraded;
    require(manager.poolInfo("pool0").health == expectedHealth, "pool backing health not exposed");
    rejects<std::runtime_error>([&] { write(manager, "lun0", 64, data); }, "unhealthy RAID allowed allocation");
    rejects<std::runtime_error>([&] { write(manager, "lun0", 14, data); }, "unhealthy RAID allowed mapped write");
    if (level == RaidLevel::Raid0) {
        rejects<std::runtime_error>([&] { read(manager, "lun0", 14, 4); }, "failed RAID exposed mapped data");
        rejects<std::runtime_error>([&] { read(manager, "lun0", 64); }, "failed RAID exposed unmapped zeros");
        return;
    }
    require(read(manager, "lun0", 14, 4) == data && read(manager, "lun0", 64) == Buffer(raidBlockSize),
            "degraded RAID did not return correct data");
    {
        StorageEngine engine;
        engine.initialize(fixture.root.path());
        engine.disks().createDisk("disk9", 64 * raidBlockSize);
        engine.arrays().registerSpare("disk9");
    }
    RebuildManager rebuild(fixture.root.path());
    const auto job = rebuild.prepare("array0", "disk0", "disk9");
    require(rebuild.run(job.id, 1).status == RebuildStatus::Paused && read(manager, "lun0", 14, 4) == data,
            "paused rebuild impaired LUN reads");
    rejects<std::runtime_error>([&] { write(manager, "lun0", 64, data); }, "rebuilding RAID allowed writes");
    require(rebuild.run(job.id).status == RebuildStatus::Completed &&
            manager.poolInfo("pool0").health == RaidHealth::Healthy && read(manager, "lun0", 14, 4) == data,
            "rebuild changed pool identity or data");
    write(manager, "lun0", 64, data);
    require(read(manager, "lun0", 64, 4) == data, "pool did not write after rebuild");
}

void testInconsistent() {
    Fixture fixture;
    fixture.provision();
    const auto path = fixture.root.path() / "data/metadata/raid_arrays/array0.pending";
    writeText(path, "schema_version=1\nmirror_write=pending\n");
    require(fixture.manager.poolInfo("pool0").health == RaidHealth::Inconsistent, "marker was ignored");
    rejects<std::runtime_error>([&] { read(fixture.manager, "lun0", 0); }, "inconsistent RAID allowed zero read");
    rejects<std::runtime_error>([&] { write(fixture.manager, "lun0", 0, binary(1, 4)); },
                                "inconsistent RAID allowed allocation");
    require(fixture.manager.poolInfo("pool0").allocatedChunks == 0 && fs::exists(path),
            "inconsistency marker or allocation changed");

    Fixture unhealthy;
    writeText(unhealthy.root.path() / "data/metadata/raid_arrays/array0.pending",
              "schema_version=1\nmirror_write=pending\n");
    rejects<std::runtime_error>([&] { unhealthy.manager.createPool("pool0", "array0"); },
                                "inconsistent RAID could be claimed");
    rejects<std::invalid_argument>([&] { unhealthy.manager.createPool("pool0", "array9"); }, "nonexistent RAID");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "interrupt-after-write") {
            ProvisioningManager manager(fs::path(argv[2]), [](auto phase) {
                if (phase == AllocationPhase::ChunkWritten) {
                    std::_Exit(42);
                }
            });
            write(manager, "lun0", 63, binary(2, 71));
            throw std::runtime_error("Child failed to exit after its first RAID write");
        }
        const std::string_view mode = argc == 2 ? argv[1] : "";
        if (mode == "data") { testData(); }
        else if (mode == "capacity") { testCapacity(); }
        else if (mode == "metadata") { testMetadata(); testLegacy(); }
        else if (mode == "faults") { testFaults(); testPublicationFailure(); testTransientPublicationDenial(); }
        else if (mode == "concurrent") { testConcurrent(); }
        else if (mode == "raid0") { testIntegration(RaidLevel::Raid0); }
        else if (mode == "raid1") { testIntegration(RaidLevel::Raid1); }
        else if (mode == "raid5") { testIntegration(RaidLevel::Raid5); }
        else if (mode == "raid6") { testIntegration(RaidLevel::Raid6); }
        else if (mode == "inconsistent") { testInconsistent(); }
        else { throw std::invalid_argument("Unknown provisioning test"); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
