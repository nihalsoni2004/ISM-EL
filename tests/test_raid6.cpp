#include "core/engine.h"
#include "raid/gf256.h"
#include "raid/parity.h"
#include "raid/raid6.h"
#include "test_support.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>

namespace {

namespace fs = std::filesystem;
using raid::ParityBlock;
using raid::raidBlockSize;
using raid::RaidLevel;
using raid::RaidStatus;
using test::rejects;
using test::require;
constexpr std::uint64_t memberBlocks = 64;
constexpr auto memberBytes = memberBlocks * raidBlockSize;

std::vector<std::uint8_t> pattern(std::uint64_t blocks, std::uint32_t seed = 123) {
    std::vector<std::uint8_t> bytes(blocks * raidBlockSize);
    for (auto& byte : bytes) {
        seed = seed * 1664525U + 1013904223U;
        byte = static_cast<std::uint8_t>(seed >> 24);
    }
    return bytes;
}

ParityBlock readPhysical(raid::VirtualDisk& disk, std::uint64_t lba) {
    ParityBlock block{};
    disk.readBlocks(lba, 1, block.data(), block.size());
    return block;
}

struct Fixture {
    test::TempDirectory root;
    std::vector<std::shared_ptr<raid::VirtualDisk>> disks;
    std::vector<std::string> ids;

    explicit Fixture(std::size_t members) {
        for (std::size_t index = 0; index < members; ++index) {
            ids.push_back("disk" + std::to_string(index));
            auto disk = std::make_shared<raid::VirtualDisk>();
            disk->create(root.path() / (ids.back() + ".img"), memberBytes);
            disks.push_back(std::move(disk));
        }
    }

    raid::RaidConfiguration configuration(std::uint64_t stripe) const {
        return {"array0", RaidLevel::Raid6, ids, stripe, (disks.size() - 2) * memberBlocks,
                raidBlockSize, raid::raidSchemaVersion, std::string(raid::raid6Layout)};
    }
};

void testGeometry() {
    for (std::size_t members : {4U, 5U, 6U}) {
        Fixture fixture(members);
        for (auto unit : std::array<std::uint64_t, 2>{1, raid::defaultStripeUnitBlocks}) {
            raid::Raid6 array(fixture.configuration(unit), fixture.disks);
            require(array.capacityBlocks() == (members - 2) * memberBlocks &&
                    array.rawCapacityBytes() == members * memberBytes, "incorrect RAID 6 geometry");
            std::uint64_t lba = 0;
            for (std::uint64_t row = 0; row < memberBlocks / unit; ++row) {
                const auto p = members - 1 - row % members;
                const auto q = p == 0 ? members - 1 : p - 1;
                std::size_t dataIndex = 0;
                for (std::size_t disk = 0; disk < members; ++disk) {
                    if (disk == p || disk == q) {
                        continue;
                    }
                    for (std::uint64_t offset = 0; offset < unit; ++offset, ++lba) {
                        const auto map = array.inspectMapping(lba);
                        const auto physical = row * unit + offset;
                        require(map.arrayId == "array0" && map.logicalLba == lba && map.stripeRow == row &&
                                map.dataUnitIndex == dataIndex && map.data.memberIndex == disk &&
                                map.data.physicalLba == physical && map.p.memberIndex == p &&
                                map.q.memberIndex == q && map.p.physicalLba == physical &&
                                map.q.physicalLba == physical, "P/Q rotation or data position differs");
                    }
                    ++dataIndex;
                }
            }
            require(lba == array.capacityBlocks(), "mapping did not cover every logical block");
            rejects<std::out_of_range>([&] { array.inspectMapping(lba); }, "past-end mapping accepted");
        }
    }

    Fixture fixture(4);
    const auto valid = fixture.configuration(16);
    auto duplicate = fixture.disks;
    duplicate.back() = duplicate.front();
    rejects<std::invalid_argument>([&] { raid::Raid6 array(valid, duplicate); }, "duplicate member accepted");
    rejects<std::invalid_argument>([&] { raid::Raid6 array(fixture.configuration(0), fixture.disks); },
                                   "zero stripe unit accepted");
    rejects<std::invalid_argument>([&] { raid::Raid6 array(fixture.configuration(3), fixture.disks); },
                                   "unaligned stripe accepted");
    rejects<std::invalid_argument>([] { raid::calculateRaidCapacity(RaidLevel::Raid6, 3, memberBlocks, 1); },
                                   "three-member RAID 6 accepted");
    rejects<std::invalid_argument>([] { raid::calculateRaidCapacity(RaidLevel::Raid6, 258, memberBlocks, 1); },
                                   "repeating Q coefficients accepted");
    require(raid::calculateRaidCapacity(RaidLevel::Raid6, 257, 1, 1) == 255, "last valid width rejected");
    constexpr auto tooLarge = std::numeric_limits<std::uint64_t>::max() / raidBlockSize / 4 + 1;
    rejects<std::overflow_error>([] { raid::calculateRaidCapacity(RaidLevel::Raid6, 4, tooLarge, 1); },
                                 "raw capacity overflow accepted");
}

void verifyImages(const Fixture& fixture, std::uint64_t unit, const std::vector<std::uint8_t>& expected) {
    const auto members = fixture.disks.size();
    const auto width = (members - 2) * unit;
    for (std::uint64_t row = 0; row < memberBlocks / unit; ++row) {
        const auto pDisk = members - 1 - row % members;
        const auto qDisk = (pDisk + members - 1) % members;
        for (std::uint64_t offset = 0; offset < unit; ++offset) {
            const auto physical = row * unit + offset;
            ParityBlock p{}, q{};
            auto logical = row * width + offset;
            std::uint8_t weight = 1;
            for (std::size_t disk = 0; disk < members; ++disk) {
                if (disk == pDisk || disk == qDisk) {
                    continue;
                }
                const auto* data = expected.data() + logical * raidBlockSize;
                const auto actual = readPhysical(*fixture.disks[disk], physical);
                require(std::equal(actual.begin(), actual.end(), data), "stored RAID 6 data differs");
                for (std::size_t byte = 0; byte < p.size(); ++byte) {
                    p[byte] ^= data[byte];
                    q[byte] ^= raid::gf256::multiply(data[byte], weight);
                }
                logical += unit;
                weight = raid::gf256::multiply(weight, 2);
            }
            require(readPhysical(*fixture.disks[pDisk], physical) == p, "stored P parity differs");
            require(readPhysical(*fixture.disks[qDisk], physical) == q, "stored Q parity differs");
        }
    }
}

void testWrites() {
    Fixture fixture(4);
    constexpr std::uint64_t unit = 16;
    raid::Raid6 array(fixture.configuration(unit), fixture.disks);
    const auto width = array.blocksPerRow();
    std::vector<std::uint8_t> expected(array.capacityBlocks() * raidBlockSize);

    const auto write = [&](std::uint64_t lba, std::uint64_t count, std::uint32_t seed) {
        const auto data = pattern(count, seed);
        array.writeBlocks(lba, count, data.data(), data.size());
        std::copy(data.begin(), data.end(), expected.data() + lba * raidBlockSize);
        return array.lastOperation();
    };
    auto report = array.verifyParity();
    require(report.rowsChecked == 4 && report.rowsWithErrors == 0, "zero-filled array has incorrect parity");
    auto counts = write(0, 1, 1);
    require(counts.logicalBlocks == 1 && counts.physicalBlockReads == 3 && counts.physicalBlockWrites == 3 &&
            counts.readRequests == 3 && counts.writeRequests == 3 &&
            counts.fullStripeRows == 0 && counts.partialStripeRows == 1, "single-block write counters");

    counts = write(width - 2, width + 5, 2);
    require(counts.logicalBlocks == 37 && counts.physicalBlockReads == 15 && counts.readRequests == 15 &&
            counts.physicalBlockWrites == 79 && counts.writeRequests == 19 &&
            counts.fullStripeRows == 1 && counts.partialStripeRows == 2, "mixed row write counters");

    counts = write(width, width, 3);
    require(counts.logicalBlocks == 32 && counts.physicalBlockReads == 0 && counts.readRequests == 0 &&
            counts.physicalBlockWrites == 64 && counts.writeRequests == 4 &&
            counts.fullStripeRows == 1 && counts.partialStripeRows == 0, "full-row write counters");

    write(0, 1, 4);
    write(array.capacityBlocks() - 1, 1, 5);
    std::vector<std::uint8_t> actual(expected.size());
    array.readBlocks(0, array.capacityBlocks(), actual.data(), actual.size());
    require(actual == expected && array.lastOperation().physicalBlockReads == array.capacityBlocks() &&
            array.lastOperation().readRequests == 8, "cross-unit logical read differs");
    array.readBlocks(0, 1, actual.data(), raidBlockSize);
    require(std::equal(actual.data(), actual.data() + raidBlockSize, expected.data()), "single-block read differs");
    rejects<std::out_of_range>([&] { array.readBlocks(array.capacityBlocks(), 1, actual.data(), actual.size()); },
                               "past-end read accepted");
    rejects<std::out_of_range>([&] { array.writeBlocks(1, UINT64_MAX, actual.data(), actual.size()); },
                               "overflowing write accepted");
    rejects<std::invalid_argument>([&] { array.readBlocks(0, 1, actual.data(), 1); }, "short read buffer accepted");
    verifyImages(fixture, unit, expected);

    report = array.verifyParity();
    require(report.rowsChecked == 4 && report.pMismatches == 0 && report.qMismatches == 0,
            "valid P or Q rejected");
    const auto corrupt = [&](raid::RaidLocation location) {
        auto block = readPhysical(*fixture.disks[location.memberIndex], location.physicalLba);
        block[13] ^= 0xFF;
        fixture.disks[location.memberIndex]->writeBlocks(location.physicalLba, 1, block.data(), block.size());
    };
    corrupt(array.inspectMapping(0).p);
    corrupt(array.inspectMapping(1).p);
    corrupt(array.inspectMapping(width).q);
    const auto both = array.inspectMapping(2 * width);
    corrupt(both.p);
    corrupt(both.q);
    report = array.verifyParity();
    require(report.rowsChecked == 4 && report.pMismatches == 2 && report.qMismatches == 2 &&
            report.rowsWithErrors == 3 && array.status() == RaidStatus::Inconsistent,
            "P and Q mismatch row counts differ");
}

void testDegraded() {
    for (std::size_t members : {4U, 5U}) {
        Fixture fixture(members);
        const auto configuration = fixture.configuration(1);
        raid::Raid6 writer(configuration, fixture.disks);
        const auto expected = pattern(writer.capacityBlocks());
        writer.writeBlocks(0, writer.capacityBlocks(), expected.data(), expected.size());

        std::vector<std::uint8_t> actual(expected.size());
        for (std::size_t first = 0; first < members; ++first) {
            raid::Raid6 single(configuration, fixture.disks);
            single.markUnavailable(fixture.ids[first]);
            single.readBlocks(0, single.capacityBlocks(), actual.data(), actual.size());
            require(actual == expected && single.status() == RaidStatus::Degraded, "single-member recovery differs");

            for (std::size_t second = first + 1; second < members; ++second) {
                raid::Raid6 pair(configuration, fixture.disks);
                pair.markUnavailable(fixture.ids[first]);
                pair.markUnavailable(fixture.ids[second]);
                std::fill(actual.begin(), actual.end(), std::uint8_t{0});
                pair.readBlocks(0, pair.capacityBlocks(), actual.data(), actual.size());
                require(actual == expected && pair.status() == RaidStatus::Degraded,
                        "two-member recovery differs across rotating rows");
                rejects<std::runtime_error>([&] { pair.writeBlocks(0, 1, actual.data(), actual.size()); },
                                            "degraded write accepted");
                rejects<std::runtime_error>([&] { pair.verifyParity(); }, "degraded array verified");
                const auto third = (second + 1) % members == first ? (second + 2) % members : (second + 1) % members;
                pair.markUnavailable(fixture.ids[third]);
                require(pair.status() == RaidStatus::Unavailable, "three unavailable members not reported");
                rejects<std::runtime_error>([&] { pair.readBlocks(0, 1, actual.data(), actual.size()); },
                                            "read with three unavailable members returned data");
            }
        }
        verifyImages(fixture, 1, expected);
        require(writer.status() == RaidStatus::Healthy, "degradation leaked between sessions");
    }
}

const std::vector<std::string> memberIds{"disk0", "disk1", "disk2", "disk3"};

void createArray(raid::StorageEngine& engine, const fs::path& root) {
    require(engine.initialize(root), "engine initialization failed");
    for (const auto& id : memberIds) {
        engine.disks().createDisk(id, memberBytes);
    }
    engine.arrays().createArray("array0", RaidLevel::Raid6, memberIds, 1);
}

void requireInconsistentAfterRestart(const fs::path& root) {
    raid::StorageEngine engine;
    require(engine.initialize(root), "restart initialization failed");
    auto& array = engine.arrays().openArray("array0");
    require(array.status() == RaidStatus::Inconsistent, "restart lost the RAID 6 marker");
    ParityBlock block{};
    rejects<std::runtime_error>([&] { array.readBlocks(0, 1, block.data(), block.size()); }, "inconsistent read allowed");
    rejects<std::runtime_error>([&] { array.writeBlocks(0, 1, block.data(), block.size()); }, "inconsistent write allowed");
    rejects<std::runtime_error>([&] { array.verify(); }, "inconsistent array verified");
}

void testPersistence() {
    test::TempDirectory root;
    raid::StorageEngine engine;
    createArray(engine, root.path());
    auto& array = engine.arrays().openArray("array0");
    const auto expected = pattern(array.capacityBlocks());
    array.writeBlocks(0, array.capacityBlocks(), expected.data(), expected.size());
    require(engine.shutdown(), "shutdown failed");

    raid::StorageEngine reopened;
    require(reopened.initialize(root.path()), "restart initialization failed");
    auto& fresh = reopened.arrays().openArray("array0");
    std::vector<std::uint8_t> actual(expected.size());
    fresh.readBlocks(0, fresh.capacityBlocks(), actual.data(), actual.size());
    require(actual == expected, "binary contents differed after restart");
    fresh.verify();
}

struct CloseFile {
    void operator()(void* handle) const noexcept { CloseHandle(handle); }
};

void testWriteFailures() {
    // Row 0 writes data to disks 0 and 1, P to disk 3, then Q to disk 2.
    const std::array<std::pair<std::size_t, std::uint64_t>, 3> failures{{{0, 0}, {3, 2}, {2, 3}}};
    for (const auto& [failedMember, completedWrites] : failures) {
        test::TempDirectory root;
        raid::StorageEngine engine;
        createArray(engine, root.path());
        auto& array = dynamic_cast<raid::Raid6&>(engine.arrays().openArray("array0"));
        const auto data = pattern(2);
        const auto path = root.path() / "data" / "virtual_disks" / (memberIds[failedMember] + ".img");
        {
            const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            nullptr, OPEN_EXISTING, 0, nullptr);
            require(handle != INVALID_HANDLE_VALUE, "unable to open member to lock test block");
            const std::unique_ptr<void, CloseFile> lock(handle);
            OVERLAPPED region{};
            require(LockFileEx(lock.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                                static_cast<DWORD>(raidBlockSize), 0, &region) != 0, "unable to lock test block");
            try {
                array.writeBlocks(0, 2, data.data(), data.size());
                throw std::logic_error("injected write failure returned success");
            } catch (const std::runtime_error& error) {
                const auto expected = "RAID 6 write failed on " + memberIds[failedMember];
                require(std::string_view(error.what()).find(expected) != std::string_view::npos,
                        "unexpected error from injected write failure");
            }
        }
        require(array.status() == RaidStatus::Inconsistent && array.lastOperation().physicalBlockReads == 0 &&
                array.lastOperation().physicalBlockWrites == completedWrites, "incorrect failure state or counters");
        require(engine.shutdown(), "shutdown after failure failed");
        requireInconsistentAfterRestart(root.path());
    }
}

void testFlushFailure() {
    test::TempDirectory root;
    {
        raid::StorageEngine setup;
        createArray(setup, root.path());
        require(setup.shutdown(), "initial shutdown failed");
    }

    const auto marker = root.path() / "data" / "metadata" / "raid_arrays" / "array0.pending";
    {
        std::vector<std::shared_ptr<raid::VirtualDisk>> disks;
        for (const auto& id : memberIds) {
            auto disk = std::make_shared<raid::VirtualDisk>();
            disk->open(root.path() / "data" / "virtual_disks" / (id + ".img"));
            disks.push_back(std::move(disk));
        }
        const raid::RaidConfiguration config{"array0", RaidLevel::Raid6, memberIds, 1, 2 * memberBlocks,
                                             raidBlockSize, raid::raidSchemaVersion, std::string(raid::raid6Layout)};
        bool completed = false;
        raid::RaidWriteJournal journal{
            [&] {
                std::ofstream file(marker, std::ios::binary);
                file << "schema_version=1\nparity_write=pending\n";
                file.close();
                require(!file.fail(), "could not create flush test marker");
            },
            [&] { completed = true; }};
        raid::Raid6 array(config, disks, journal, false, [&](raid::VirtualDisk& disk) {
            require(fs::is_regular_file(marker), "marker missing when flush began");
            if (&disk == disks[2].get()) {
                throw std::runtime_error("injected member flush failure");
            }
            disk.flush();
        });
        const auto data = pattern(2);
        try {
            array.writeBlocks(0, 2, data.data(), data.size());
            throw std::logic_error("injected flush failure returned success");
        } catch (const std::runtime_error& error) {
            require(std::string_view(error.what()).find("RAID 6 flush failed on disk2") != std::string_view::npos,
                    "flush failure lost the member diagnosis");
        }
        require(array.status() == RaidStatus::Inconsistent && !completed && fs::exists(marker) &&
                array.lastOperation().physicalBlockWrites == 4, "flush failure cleared marker or missed writes");
    }
    requireInconsistentAfterRestart(root.path());
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view name = argc == 2 ? argv[1] : "";
        if (name == "geometry") {
            testGeometry();
        } else if (name == "writes") {
            testWrites();
        } else if (name == "degraded") {
            testDegraded();
        } else if (name == "persistence") {
            testPersistence();
        } else if (name == "write-failures") {
            testWriteFailures();
        } else if (name == "flush-failure") {
            testFlushFailure();
        } else {
            throw std::invalid_argument("Unknown RAID 6 test");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
