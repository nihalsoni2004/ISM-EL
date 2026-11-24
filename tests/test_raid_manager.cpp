#include "core/engine.h"
#include "test_support.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using test::rejects;
using test::require;
using raid::RaidLevel;
using raid::raidBlockSize;
using Block = std::array<std::uint8_t, raidBlockSize>;
constexpr std::uint64_t memberBlocks = 64;

struct Fixture {
    test::TempDirectory root;
    fs::path catalog = root.path() / "data" / "metadata" / "raid_arrays";
    fs::path images = root.path() / "data" / "virtual_disks";
    raid::StorageEngine engine;

    explicit Fixture(int disks) {
        require(engine.initialize(root.path()), "engine initialization failed");
        for (int index = 0; index < disks; ++index) {
            engine.disks().createDisk("disk" + std::to_string(index), memberBlocks * raidBlockSize);
        }
    }
};

void putMetadata(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << text;
    file.close();
    require(!file.fail(), "could not write metadata fixture");
}

std::string metadata(std::string_view id, std::string_view members, std::uint64_t schema = 1,
                     std::uint64_t capacity = 128, std::uint64_t stripe = 16) {
    return "schema_version=" + std::to_string(schema) + "\narray_id=" + std::string(id) +
           "\nraid_level=0\nmembers=" + std::string(members) + "\nblock_size=4096\nstripe_unit_blocks=" +
           std::to_string(stripe) + "\ncapacity_blocks=" + std::to_string(capacity) + '\n';
}

Block readPhysical(raid::VirtualDisk& disk, std::uint64_t lba) {
    Block data{};
    disk.readBlocks(lba, 1, data.data(), data.size());
    return data;
}

void testManager() {
    Fixture fixture(6);
    auto& manager = fixture.engine.arrays();
    require(manager.listArrays().empty(), "new catalog was not empty");
    rejects<std::invalid_argument>([&] { manager.createArray("../array0", RaidLevel::Raid0, {"disk0", "disk1"}, 16); },
                                   "unsafe array identifier accepted");
    rejects<std::invalid_argument>([&] { manager.createArray("array0", RaidLevel::Raid0, {"../disk0", "disk1"}, 16); },
                                   "unsafe member identifier accepted");
    rejects<std::invalid_argument>([&] { manager.createArray("array0", RaidLevel::Raid0, {"disk0", "disk0"}, 16); },
                                   "duplicate members accepted");
    rejects<std::runtime_error>([&] { manager.createArray("array0", RaidLevel::Raid0, {"disk0", "disk9"}, 16); },
                                "missing image accepted");
    rejects<std::invalid_argument>([&] { manager.createArray("array0", static_cast<RaidLevel>(9),
                                                             {"disk0", "disk1"}, 16); },
                                   "unsupported level accepted");
    fixture.engine.disks().createDisk("disk6", 2 * memberBlocks * raidBlockSize);
    rejects<std::invalid_argument>([&] { manager.createArray("array0", RaidLevel::Raid0, {"disk0", "disk6"}, 16); },
                                   "unequal capacities accepted");

    Block payload{};
    const std::string utf8 = "RAID \xE9\x9B\xAA";
    std::copy(utf8.begin(), utf8.end(), payload.begin());
    auto& used = fixture.engine.disks().openDisk("disk5");
    used.writeBlocks(63, 1, payload.data(), payload.size());
    rejects<std::invalid_argument>([&] { manager.createArray("array0", RaidLevel::Raid1, {"disk4", "disk5"}, 1); },
                                   "nonzero member was claimed");
    require(readPhysical(used, 63) == payload, "failed creation altered preexisting data");
    require(readPhysical(fixture.engine.disks().openDisk("disk4"), 63) == Block{},
            "failed creation altered the other member");
    for (const auto& entry : fs::directory_iterator(fixture.catalog)) {
        require(entry.path().filename() == ".catalog.lock", "failed creation published metadata or left staging");
    }

    auto& striped = manager.createArray("array0", RaidLevel::Raid0, {"disk1", "disk0"}, 16);
    auto& mirrored = manager.createArray("array1", RaidLevel::Raid1, {"disk2", "disk3"}, 1);
    require(striped.capacityBlocks() == 128 && mirrored.capacityBlocks() == 64, "incorrect array capacities");
    require(fs::is_regular_file(fixture.catalog / "array0.raid") && fs::is_regular_file(fixture.catalog / "array1.raid"),
            "array metadata files were not published");
    require(manager.listArrays().size() == 2, "catalog did not list both arrays");
    rejects<std::invalid_argument>([&] { manager.createArray("array0", RaidLevel::Raid0, {"disk4", "disk6"}, 16); },
                                   "duplicate array accepted");
    rejects<std::invalid_argument>([&] { manager.createArray("array2", RaidLevel::Raid0, {"disk4", "disk0"}, 16); },
                                   "member ownership conflict accepted");
    rejects<std::invalid_argument>([&] { manager.requireUnownedDisk("disk0"); }, "owned raw disk write accepted");
    manager.requireUnownedDisk("disk4");
    rejects<std::logic_error>([&] { fixture.engine.disks().closeDisk("disk0"); }, "leased member closed through manager");

    striped.writeBlocks(20, 1, payload.data(), payload.size());
    mirrored.writeBlocks(20, 1, payload.data(), payload.size());
    require(!fs::exists(fixture.catalog / "array1.pending"), "successful write left a pending marker");
    require(fixture.engine.shutdown(), "first engine did not shut down");

    raid::StorageEngine restarted;
    require(restarted.initialize(fixture.root.path()), "fresh engine did not initialize");
    require(restarted.arrays().listArrays().size() == 2, "fresh engine lost metadata");
    for (const auto& id : {"array0", "array1"}) {
        auto& array = restarted.arrays().openArray(id);
        Block actual{};
        array.readBlocks(20, 1, actual.data(), actual.size());
        require(actual == payload, "data differs after opening persisted array in a fresh engine");
        array.verify();
    }
    require(readPhysical(restarted.disks().openDisk("disk0"), 4) == payload &&
            readPhysical(restarted.disks().openDisk("disk1"), 4) == Block{}, "stripe order did not persist");
    require(readPhysical(restarted.disks().openDisk("disk2"), 20) == payload &&
            readPhysical(restarted.disks().openDisk("disk3"), 20) == payload, "mirrors differ on disk");
    require(restarted.shutdown(), "fresh engine did not shut down");
}

void testMetadata() {
    Fixture fixture(3);
    auto& manager = fixture.engine.arrays();
    const auto path = fixture.catalog / "array0.raid";
    for (const auto& corrupt : {std::string("unknown\n"), metadata("array0", "disk0,disk1", 2),
                                 metadata("array1", "disk0,disk1"), metadata("array0", "../disk0,disk1"),
                                 metadata("array0", "disk0,disk0"), metadata("array0", "disk0,disk1", 1, 127),
                                 metadata("array0", "disk0,disk1", 1, 128, 0)}) {
        putMetadata(path, corrupt);
        rejects<std::invalid_argument>([&] { manager.listArrays(); }, "malformed metadata accepted");
        rejects<std::invalid_argument>([&] { manager.requireUnownedDisk("disk2"); },
                                       "raw write allowed with invalid ownership catalog");
    }

    putMetadata(path, metadata("array0", "disk0,disk9"));
    require(manager.getArrayStatus("array0").health == raid::RaidHealth::Failed,
            "RAID 0 with a missing member reported incorrect health");
    rejects<std::invalid_argument>([&] { manager.requireUnownedDisk("disk0"); }, "missing member released ownership");
    fs::remove(fixture.catalog / "array0.state");  // A separate hand-edited metadata fixture follows.

    putMetadata(path, metadata("array0", "disk0,disk1", 1, 256));
    rejects<std::invalid_argument>([&] { manager.openArray("array0"); }, "stored capacity mismatch was accepted");

    putMetadata(path, metadata("array0", "disk0,disk1"));
    const auto external = fixture.root.path() / "external.raid";
    putMetadata(external, metadata("array1", "disk1,disk2"));
    fs::create_hard_link(external, fixture.catalog / "array1.raid");
    rejects<std::invalid_argument>([&] { manager.listArrays(); }, "linked metadata file was accepted");
    fs::remove(fixture.catalog / "array1.raid");
    putMetadata(fixture.catalog / "array1.raid", metadata("array1", "disk1,disk2"));
    rejects<std::invalid_argument>([&] { manager.listArrays(); }, "conflicting ownership in metadata was accepted");
}

struct CloseFile {
    void operator()(void* handle) const noexcept { CloseHandle(handle); }
};

void testInterruptedWrite() {
    Fixture fixture(2);
    auto& array = fixture.engine.arrays().createArray("array0", RaidLevel::Raid1, {"disk0", "disk1"}, 1);
    constexpr std::uint64_t lba = 2;
    Block expected{};
    expected.fill(0xFF);
    {
        // A second handle denies writes to exactly one block of the second member.
        const auto handle = CreateFileW((fixture.images / "disk1.img").c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "could not open second member for test lock");
        const std::unique_ptr<void, CloseFile> lock(handle);
        OVERLAPPED region{};
        region.Offset = static_cast<DWORD>(lba * raidBlockSize);
        require(LockFileEx(lock.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                            static_cast<DWORD>(raidBlockSize), 0, &region) != 0, "could not lock test block");
        try {
            array.writeBlocks(lba, 1, expected.data(), expected.size());
            throw std::logic_error("Expected real I/O failure on second mirror");
        } catch (const std::runtime_error& error) {
            require(std::string_view(error.what()).find("Incomplete RAID 1 write on disk1") != std::string_view::npos,
                    "expected partial mirror write diagnosis");
        }
    }
    require(array.status() == raid::RaidStatus::Inconsistent && fs::exists(fixture.catalog / "array0.pending"),
            "partial write did not persist its pending marker");
    require(readPhysical(fixture.engine.disks().openDisk("disk0"), lba) == expected &&
            readPhysical(fixture.engine.disks().openDisk("disk1"), lba) == Block{},
            "test did not produce distinguishable mirror contents");
    require(fixture.engine.shutdown(), "shutdown after mirror failure failed");

    raid::StorageEngine restarted;
    require(restarted.initialize(fixture.root.path()), "restart did not initialize");
    auto& reopened = restarted.arrays().openArray("array0");
    require(reopened.status() == raid::RaidStatus::Inconsistent, "restart forgot the mirror write failure");
    Block recovered{};
    reopened.markUnavailable("disk0");
    rejects<std::runtime_error>([&] { reopened.readBlocks(lba, 1, recovered.data(), recovered.size()); },
                               "restart served stale data from inconsistent array");
    rejects<std::runtime_error>([&] { reopened.verify(); }, "restart verified inconsistent data");
    rejects<std::runtime_error>([&] { reopened.writeBlocks(lba, 1, expected.data(), expected.size()); },
                               "restart allowed new writes to inconsistent data");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view test = argc == 2 ? argv[1] : "";
        if (test == "manager") {
            testManager();
        } else if (test == "metadata") {
            testMetadata();
        } else if (test == "interrupted") {
            testInterruptedWrite();
        } else {
            throw std::invalid_argument("Unknown RAID manager test");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
