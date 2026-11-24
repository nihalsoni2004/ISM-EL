#include "core/engine.h"
#include "raid/parity.h"
#include "raid/raid5.h"
#include "test_support.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
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
            disk->create(root.path() / (ids.back() + ".img"), memberBlocks * raidBlockSize);
            disks.push_back(std::move(disk));
        }
    }

    raid::RaidConfiguration configuration(std::uint64_t stripe) const {
        return {"array0", RaidLevel::Raid5, ids, stripe, (disks.size() - 1) * memberBlocks,
                raidBlockSize, raid::raidSchemaVersion, std::string(raid::raid5Layout)};
    }
};

// Independently enumerate physical locations and build expected parity from the original data.
void verifyImages(const Fixture& fixture, std::uint64_t unit, const std::vector<std::uint8_t>& expected) {
    const auto members = fixture.disks.size();
    const auto width = (members - 1) * unit;
    for (std::uint64_t row = 0; row < memberBlocks / unit; ++row) {
        const auto parityDisk = members - 1 - row % members;
        for (std::uint64_t offset = 0; offset < unit; ++offset) {
            const auto physical = row * unit + offset;
            ParityBlock parity{};
            auto logical = row * width + offset;
            for (std::size_t disk = 0; disk < members; ++disk) {
                if (disk == parityDisk) {
                    continue;
                }
                const auto* data = expected.data() + logical * raidBlockSize;
                const auto actual = readPhysical(*fixture.disks[disk], physical);
                require(std::equal(actual.begin(), actual.end(), data), "physical RAID 5 data differs");
                for (std::size_t byte = 0; byte < parity.size(); ++byte) {
                    parity[byte] ^= data[byte];
                }
                logical += unit;
            }
            require(readPhysical(*fixture.disks[parityDisk], physical) == parity, "physical XOR parity differs");
        }
    }
}

void testGeometry() {
    for (std::size_t members : {3U, 4U, 5U}) {
        Fixture fixture(members);
        for (auto unit : std::array<std::uint64_t, 2>{1, raid::defaultStripeUnitBlocks}) {
            raid::Raid5 array(fixture.configuration(unit), fixture.disks);
            require(array.capacityBlocks() == (members - 1) * memberBlocks &&
                    array.rawCapacityBytes() == members * memberBlocks * raidBlockSize, "RAID 5 capacity");
            std::uint64_t lba = 0;
            for (std::uint64_t row = 0; row < memberBlocks / unit; ++row) {
                const auto parity = members - 1 - row % members;
                for (std::size_t disk = 0; disk < members; ++disk) {
                    if (disk == parity) {
                        continue;
                    }
                    for (std::uint64_t offset = 0; offset < unit; ++offset, ++lba) {
                        const auto mapping = array.inspectMapping(lba);
                        const auto physical = row * unit + offset;
                        require(mapping.logicalLba == lba && mapping.stripeRow == row &&
                                mapping.data.memberIndex == disk && mapping.data.physicalLba == physical &&
                                mapping.parity.memberIndex == parity && mapping.parity.physicalLba == physical,
                                "unexpected RAID 5 rotation or data position");
                    }
                }
            }
            require(lba == array.capacityBlocks(), "mapping did not cover every logical block");
            rejects<std::out_of_range>([&] { array.inspectMapping(lba); }, "past-end mapping accepted");
        }
    }

    Fixture fixture(3);
    const auto valid = fixture.configuration(16);
    rejects<std::invalid_argument>([&] { raid::Raid5 array(fixture.configuration(0), fixture.disks); },
                                   "zero stripe unit accepted");
    rejects<std::invalid_argument>([&] { raid::Raid5 array(fixture.configuration(3), fixture.disks); },
                                   "uneven stripe unit accepted");
    auto wrong = valid;
    wrong.layout = "unsupported";
    rejects<std::invalid_argument>([&] { raid::Raid5 array(wrong, fixture.disks); }, "unknown layout accepted");
    rejects<std::invalid_argument>([] { raid::calculateRaidCapacity(RaidLevel::Raid5, 2, memberBlocks, 1); },
                                   "two-member RAID 5 accepted");
    constexpr auto tooLarge = std::numeric_limits<std::uint64_t>::max() / raidBlockSize / 3 + 1;
    rejects<std::overflow_error>([] { raid::calculateRaidCapacity(RaidLevel::Raid5, 3, tooLarge, 1); },
                                 "overflowed raw capacity accepted");

    raid::Raid5 array(valid, fixture.disks);
    ParityBlock scratch{};
    rejects<std::out_of_range>([&] {
        array.writeBlocks(array.capacityBlocks() - 1, 2, scratch.data(), scratch.size());
    }, "out-of-range RAID 5 write accepted");
    rejects<std::out_of_range>([&] {
        array.readBlocks(1, UINT64_MAX, scratch.data(), scratch.size());
    }, "overflowing read request accepted");
    rejects<std::invalid_argument>([&] {
        array.readBlocks(0, 1, nullptr, scratch.size());
    }, "null read buffer accepted");
    rejects<std::invalid_argument>([&] {
        array.writeBlocks(0, 1, scratch.data(), scratch.size() - 1);
    }, "undersized write buffer accepted");
}

void testWrites() {
    Fixture fixture(4);
    constexpr std::uint64_t unit = 16;
    raid::Raid5 array(fixture.configuration(unit), fixture.disks);
    const auto width = array.blocksPerRow();
    std::vector<std::uint8_t> expected(array.capacityBlocks() * raidBlockSize);

    const auto write = [&](std::uint64_t lba, std::uint64_t count, std::uint32_t seed) {
        const auto data = pattern(count, seed);
        array.writeBlocks(lba, count, data.data(), data.size());
        std::copy(data.begin(), data.end(), expected.data() + lba * raidBlockSize);
        return array.lastOperation();
    };
    auto result = array.verifyParity();
    require(result.rowsChecked == 4 && result.parityMismatches == 0, "zero-filled initialization has bad parity");

    auto counts = write(0, 1, 1);
    require(counts.logicalBlocks == 1 && counts.physicalBlockReads == 2 && counts.physicalBlockWrites == 2 &&
            counts.readRequests == 2 && counts.writeRequests == 2 &&
            counts.partialStripeRows == 1 && counts.fullStripeRows == 0, "single-block counts");

    counts = write(width - 2, width + 5, 2);
    require(counts.logicalBlocks == width + 5 && counts.physicalBlockReads == 10 &&
            counts.physicalBlockWrites == 74 && counts.readRequests == 10 && counts.writeRequests == 14 &&
            counts.fullStripeRows == 1 && counts.partialStripeRows == 2, "mixed full and partial row counts");

    counts = write(width, width, 3);
    require(counts.logicalBlocks == 48 && counts.physicalBlockReads == 0 && counts.physicalBlockWrites == 64 &&
            counts.readRequests == 0 && counts.writeRequests == 4 &&
            counts.fullStripeRows == 1 && counts.partialStripeRows == 0, "full row read existing data or miscounted");

    write(0, 1, 4);
    write(array.capacityBlocks() - 1, 1, 5);
    std::vector<std::uint8_t> actual(expected.size());
    array.readBlocks(0, array.capacityBlocks(), actual.data(), actual.size());
    require(actual == expected && array.lastOperation().physicalBlockReads == array.capacityBlocks() &&
            array.lastOperation().readRequests == 12, "batched logical read differs");
    verifyImages(fixture, unit, expected);
    result = array.verifyParity();
    require(result.rowsChecked == 4 && result.parityMismatches == 0, "valid stored parity rejected");

    // Corrupt two parity blocks in the same row and one in the final row.
    for (auto lba : std::array<std::uint64_t, 3>{0, 1, array.capacityBlocks() - 1}) {
        const auto parity = array.inspectMapping(lba).parity;
        auto corrupted = readPhysical(*fixture.disks[parity.memberIndex], parity.physicalLba);
        corrupted[13] ^= 0xFF;
        fixture.disks[parity.memberIndex]->writeBlocks(parity.physicalLba, 1, corrupted.data(), corrupted.size());
    }
    result = array.verifyParity();
    require(result.rowsChecked == 4 && result.parityMismatches == 2 &&
            array.status() == RaidStatus::Inconsistent, "verification failed to count inconsistent rows");
}

void testDegraded() {
    Fixture fixture(4);
    const auto configuration = fixture.configuration(1);
    raid::Raid5 writer(configuration, fixture.disks);
    const auto expected = pattern(writer.capacityBlocks());
    writer.writeBlocks(0, writer.capacityBlocks(), expected.data(), expected.size());

    for (std::size_t missing = 0; missing < fixture.disks.size(); ++missing) {
        raid::Raid5 reader(configuration, fixture.disks);
        reader.markUnavailable(fixture.ids[missing]);
        require(reader.status() == RaidStatus::Degraded, "one unavailable disk did not degrade RAID 5");
        std::vector<std::uint8_t> recovered(expected.size());
        reader.readBlocks(0, reader.capacityBlocks(), recovered.data(), recovered.size());
        require(recovered == expected, "degraded multi-block read changed binary data");
        const auto first = reader.inspectMapping(0);
        ParityBlock block{};
        reader.readBlocks(0, 1, block.data(), block.size());
        require(reader.lastOperation().physicalBlockReads == (first.data.memberIndex == missing ? 3 : 1),
                "read should reconstruct only when the data member is unavailable");
        rejects<std::runtime_error>([&] { reader.writeBlocks(0, 1, block.data(), block.size()); },
                                    "degraded write accepted");
        rejects<std::runtime_error>([&] { reader.verifyParity(); }, "degraded array verified");
        reader.markUnavailable(fixture.ids[(missing + 1) % fixture.ids.size()]);
        require(reader.status() == RaidStatus::Unavailable, "two missing members not reported");
        rejects<std::runtime_error>([&] { reader.readBlocks(0, 1, block.data(), block.size()); },
                                    "two-member failure returned data");
    }
    verifyImages(fixture, 1, expected);
    require(writer.status() == RaidStatus::Healthy, "session change leaked to another controller");
}

struct CloseFile {
    void operator()(void* handle) const noexcept { CloseHandle(handle); }
};

void testInterruptedWrite() {
    test::TempDirectory root;
    const auto images = root.path() / "data" / "virtual_disks";
    const auto marker = root.path() / "data" / "metadata" / "raid_arrays" / "array0.pending";
    raid::StorageEngine engine;
    require(engine.initialize(root.path()), "engine initialization failed");
    for (const auto* id : {"disk0", "disk1", "disk2"}) {
        engine.disks().createDisk(id, memberBlocks * raidBlockSize);
    }
    auto& array = dynamic_cast<raid::Raid5&>(
        engine.arrays().createArray("array0", RaidLevel::Raid5, {"disk0", "disk1", "disk2"}, 1));
    const auto data = pattern(2);
    {
        // Full row writes bypass parity reads, so this lock fails precisely at the parity write.
        const auto handle = CreateFileW((images / "disk2.img").c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "unable to open parity image for test lock");
        const std::unique_ptr<void, CloseFile> lock(handle);
        OVERLAPPED region{};
        require(LockFileEx(lock.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                            static_cast<DWORD>(raidBlockSize), 0, &region) != 0, "unable to lock parity block");
        try {
            array.writeBlocks(0, 2, data.data(), data.size());
            throw std::logic_error("Parity write failure returned success");
        } catch (const std::runtime_error& error) {
            require(std::string_view(error.what()).find("RAID 5 write failed on disk2") != std::string_view::npos,
                    "unexpected parity write error");
        }
    }
    require(array.lastOperation().physicalBlockReads == 0 && array.lastOperation().physicalBlockWrites == 2,
            "failed parity write did not follow two completed data writes");
    require(array.status() == RaidStatus::Inconsistent && fs::exists(marker), "failed write marker missing");
    for (std::size_t disk = 0; disk < 2; ++disk) {
        const auto block = readPhysical(engine.disks().openDisk("disk" + std::to_string(disk)), 0);
        require(std::equal(block.begin(), block.end(), data.data() + disk * raidBlockSize),
                "expected partial data update was not written");
    }
    require(readPhysical(engine.disks().openDisk("disk2"), 0) == ParityBlock{}, "locked parity was overwritten");
    require(engine.shutdown(), "first shutdown failed");

    raid::StorageEngine reopened;
    require(reopened.initialize(root.path()), "restart initialization failed");
    auto& damaged = reopened.arrays().openArray("array0");
    require(damaged.status() == RaidStatus::Inconsistent, "restart lost the write marker");
    ParityBlock buffer{};
    rejects<std::runtime_error>([&] { damaged.readBlocks(0, 1, buffer.data(), buffer.size()); },
                                "inconsistent RAID 5 served data");
    rejects<std::runtime_error>([&] { damaged.writeBlocks(0, 1, buffer.data(), buffer.size()); },
                                "inconsistent RAID 5 accepted writes");
    rejects<std::runtime_error>([&] { damaged.verify(); }, "inconsistent RAID 5 verified");
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
        } else if (name == "interrupted") {
            testInterruptedWrite();
        } else {
            throw std::invalid_argument("Unknown RAID 5 test");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
