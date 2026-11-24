#include "raid/raid0.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

using test::rejects;
using test::require;
using raid::raidBlockSize;
using Block = std::array<std::uint8_t, raidBlockSize>;
constexpr std::uint64_t memberBlocks = 64;

struct Fixture {
    test::TempDirectory directory;
    std::vector<std::shared_ptr<raid::VirtualDisk>> disks;
    std::vector<std::string> ids;

    explicit Fixture(std::size_t count) {
        for (std::size_t index = 0; index < count; ++index) {
            ids.push_back("disk" + std::to_string(index));
            auto disk = std::make_shared<raid::VirtualDisk>();
            disk->create(directory.path() / (ids.back() + ".img"), memberBlocks * raidBlockSize);
            disks.push_back(std::move(disk));
        }
    }

    raid::RaidConfiguration config(std::uint64_t stripe) const {
        return {"array0", raid::RaidLevel::Raid0, ids, stripe, disks.size() * memberBlocks};
    }
};

Block read(raid::VirtualDisk& disk, std::uint64_t lba) {
    Block block{};
    disk.readBlocks(lba, 1, block.data(), block.size());
    return block;
}

void testDefaultStripe() {
    Fixture fixture(2);
    raid::Raid0 array(fixture.config(raid::defaultStripeUnitBlocks), fixture.disks);
    require(array.capacityBlocks() == 128, "two-member RAID 0 capacity");
    const auto check = [&](auto lba, auto index, auto physical) {
        const auto mapping = array.map(lba);
        require(mapping.size() == 1 && mapping.front().memberIndex == index &&
                mapping.front().physicalLba == physical, "incorrect RAID 0 mapping");
    };
    check(0, 0U, 0U);
    check(15, 0U, 15U);
    check(16, 1U, 0U);
    check(31, 1U, 15U);
    check(32, 0U, 16U);
    check(127, 1U, 63U);

    constexpr std::uint64_t first = 14, blocks = 22;
    std::vector<std::uint8_t> expected(blocks * raidBlockSize);
    std::uint32_t seed = 0x12345678;
    for (auto& byte : expected) {
        seed = seed * 1664525U + 1013904223U;
        byte = static_cast<std::uint8_t>(seed >> 24);
    }
    array.writeBlocks(first, blocks, expected.data(), expected.size());
    std::vector<std::uint8_t> actual(expected.size());
    array.readBlocks(first, blocks, actual.data(), actual.size());
    require(actual == expected, "multi-stripe round trip changed bytes");

    // Independently verify each member's physical contents instead of consulting map().
    for (std::uint64_t index = 0; index < blocks; ++index) {
        const auto lba = first + index;
        const auto disk = lba >= 16 && lba < 32 ? 1U : 0U;
        const auto physical = lba < 16 ? lba : lba - 16;
        const auto block = read(*fixture.disks[disk], physical);
        require(std::equal(block.begin(), block.end(), expected.data() + index * raidBlockSize),
                "physical block differs from expected striped payload");
    }
    require(read(*fixture.disks[0], 13) == Block{} && read(*fixture.disks[0], 20) == Block{} &&
            read(*fixture.disks[1], 16) == Block{}, "stripe write altered a neighboring block");
    Block ends{};
    ends.fill(0xFF);
    array.writeBlocks(0, 1, ends.data(), ends.size());
    array.writeBlocks(127, 1, ends.data(), ends.size());
    require(read(*fixture.disks[0], 0) == ends && read(*fixture.disks[1], 63) == ends,
            "first and last blocks reached the wrong members");
    array.verify();
}

void testSingleStripe() {
    Fixture fixture(3);
    raid::Raid0 array(fixture.config(1), fixture.disks);
    require(array.capacityBlocks() == 192, "three-member RAID 0 capacity");
    std::array<std::uint8_t, 6 * raidBlockSize> pattern{};
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        pattern[index] = static_cast<std::uint8_t>((index % 256) ^ (index / raidBlockSize));
    }
    array.writeBlocks(0, 6, pattern.data(), pattern.size());
    for (std::size_t index = 0; index < 6; ++index) {
        const auto location = array.map(index).front();
        require(location.memberIndex == index % 3 && location.physicalLba == index / 3,
                "one-block stripes did not rotate across all three disks");
        const auto block = read(*fixture.disks[index % 3], index / 3);
        require(std::equal(block.begin(), block.end(), pattern.data() + index * raidBlockSize),
                "rotated block did not match physical contents");
    }
    decltype(pattern) result{};
    array.readBlocks(0, 6, result.data(), result.size());
    require(result == pattern, "one-block stripe round trip differs");
}

void testInvalidOperations() {
    Fixture fixture(2);
    Block data{};
    auto configuration = fixture.config(16);
    raid::Raid0 array(configuration, fixture.disks);
    rejects<std::out_of_range>([&] { array.map(128); }, "out-of-bounds mapping accepted");
    rejects<std::out_of_range>([&] { array.readBlocks(128, 1, data.data(), data.size()); }, "invalid read accepted");
    rejects<std::out_of_range>([&] { array.writeBlocks(127, 2, data.data(), data.size()); }, "invalid write accepted");
    rejects<std::out_of_range>([&] { array.readBlocks(1, UINT64_MAX, data.data(), data.size()); }, "overflow accepted");
    rejects<std::invalid_argument>([&] { array.readBlocks(1, 0, data.data(), data.size()); }, "zero count accepted");
    rejects<std::invalid_argument>([&] { array.writeBlocks(1, 1, data.data(), 1); }, "short buffer accepted");
    rejects<std::invalid_argument>([&] { array.readBlocks(1, 1, nullptr, data.size()); }, "null buffer accepted");
    rejects<std::invalid_argument>([&] { raid::Raid0 invalid(fixture.config(3), fixture.disks); }, "partial stripe accepted");
    rejects<std::invalid_argument>([&] { raid::Raid0 invalid(fixture.config(0), fixture.disks); }, "zero stripe accepted");
    rejects<std::invalid_argument>([&] { raid::Raid0 invalid(configuration, {fixture.disks[0], nullptr}); },
                                   "missing disk accepted");
    rejects<std::invalid_argument>([] { raid::calculateRaidCapacity(raid::RaidLevel::Raid0, 1, 64, 1); },
                                   "one-member RAID 0 accepted");
    constexpr auto tooLarge = std::numeric_limits<std::uint64_t>::max() / raidBlockSize / 2 + 1;
    rejects<std::overflow_error>([] { raid::calculateRaidCapacity(raid::RaidLevel::Raid0, 2, tooLarge, 1); },
                                 "RAID 0 capacity overflow accepted");

    array.markUnavailable("disk1");
    require(array.status() == raid::RaidStatus::Unavailable, "RAID 0 accepted a missing member");
    rejects<std::runtime_error>([&] { array.readBlocks(0, 1, data.data(), data.size()); }, "unavailable RAID 0 read");
    rejects<std::runtime_error>([&] { array.writeBlocks(0, 1, data.data(), data.size()); }, "unavailable RAID 0 write");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view test = argc == 2 ? argv[1] : "";
        if (test == "default") {
            testDefaultStripe();
        } else if (test == "single") {
            testSingleStripe();
        } else if (test == "invalid") {
            testInvalidOperations();
        } else {
            throw std::invalid_argument("Unknown RAID 0 test");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
