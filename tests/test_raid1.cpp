#include "raid/raid1.h"
#include "test_support.h"

#include <array>
#include <iostream>
#include <string_view>

namespace {

using test::rejects;
using test::require;
using raid::raidBlockSize;
using Block = std::array<std::uint8_t, raidBlockSize>;
constexpr std::uint64_t memberBlocks = 64;

struct Fixture {
    test::TempDirectory directory;
    std::shared_ptr<raid::VirtualDisk> first = std::make_shared<raid::VirtualDisk>();
    std::shared_ptr<raid::VirtualDisk> second = std::make_shared<raid::VirtualDisk>();
    raid::RaidConfiguration config{"array0", raid::RaidLevel::Raid1, {"disk0", "disk1"}, 1, memberBlocks};

    Fixture() {
        first->create(directory.path() / "disk0.img", memberBlocks * raidBlockSize);
        second->create(directory.path() / "disk1.img", memberBlocks * raidBlockSize);
    }
};

void testMirror() {
    Fixture fixture;
    raid::Raid1 array(fixture.config, {fixture.first, fixture.second});
    require(array.status() == raid::RaidStatus::Healthy && array.capacityBlocks() == memberBlocks,
            "healthy RAID 1 capacity");
    const auto mapping = array.map(63);
    require(mapping.size() == 2 && mapping[0].memberIndex == 0 && mapping[0].physicalLba == 63 &&
            mapping[1].memberIndex == 1 && mapping[1].physicalLba == 63, "mirror address mapping");
    std::array<std::uint8_t, 3 * raidBlockSize> pattern{};
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        pattern[index] = static_cast<std::uint8_t>(index);
    }
    array.writeBlocks(61, 3, pattern.data(), pattern.size());
    Block first{};
    first.fill(0xFF);
    array.writeBlocks(0, 1, first.data(), first.size());

    for (const auto& disk : {fixture.first, fixture.second}) {
        decltype(pattern) blocks{};
        Block block{};
        disk->readBlocks(61, 3, blocks.data(), blocks.size());
        disk->readBlocks(0, 1, block.data(), block.size());
        require(blocks == pattern && block == first, "mirror does not contain the written bytes");
        disk->readBlocks(60, 1, block.data(), block.size());
        require(block == Block{}, "mirror overwrote a neighboring block");
    }
    decltype(pattern) logical{};
    array.readBlocks(61, 3, logical.data(), logical.size());
    require(logical == pattern, "logical mirror read differs from original data");
    array.verify();
    rejects<std::out_of_range>([&] { array.readBlocks(64, 1, first.data(), first.size()); }, "invalid mirror LBA");
    rejects<std::out_of_range>([&] { array.writeBlocks(63, 2, first.data(), first.size()); }, "invalid mirror range");
    rejects<std::invalid_argument>([] { raid::calculateRaidCapacity(raid::RaidLevel::Raid1, 3, 64, 1); },
                                   "RAID 1 accepted three members");
    const Block zeros{};
    fixture.second->writeBlocks(63, 1, zeros.data(), zeros.size());
    rejects<std::runtime_error>([&] { array.verify(); }, "full mirror comparison missed divergence at the last LBA");
    require(array.status() == raid::RaidStatus::Inconsistent, "divergent mirror remained healthy");
}

void testDegraded() {
    Fixture fixture;
    raid::Raid1 array(fixture.config, {fixture.first, fixture.second});
    Block original{};
    original.fill(0xA5);
    array.writeBlocks(8, 1, original.data(), original.size());
    array.markUnavailable("disk0");
    require(array.status() == raid::RaidStatus::Degraded, "one unavailable member must be reported as degraded");

    // Deliberately change the disabled test copy to prove the survivor is used.
    const Block zeros{};
    fixture.first->writeBlocks(8, 1, zeros.data(), zeros.size());
    Block recovered{};
    array.readBlocks(8, 1, recovered.data(), recovered.size());
    require(recovered == original, "degraded read used the unavailable copy");
    rejects<std::runtime_error>([&] { array.writeBlocks(8, 1, zeros.data(), zeros.size()); },
                               "degraded write was accepted");
    rejects<std::runtime_error>([&] { array.verify(); }, "verified a degraded mirror");
    array.markUnavailable("disk1");
    require(array.status() == raid::RaidStatus::Unavailable, "both unavailable members must be reported");
    rejects<std::runtime_error>([&] { array.readBlocks(8, 1, recovered.data(), recovered.size()); },
                               "read succeeded without any available mirrors");
}

void testPartialWrite() {
    Fixture fixture;
    bool completed = false;
    raid::MirrorWriteJournal journal{
        [&] { fixture.second->close(); },
        [&] { completed = true; }};
    raid::Raid1 array(fixture.config, {fixture.first, fixture.second}, journal);
    Block original{};
    original.fill(0xFF);
    rejects<std::runtime_error>([&] { array.writeBlocks(2, 1, original.data(), original.size()); },
                               "partial mirror write reported success");
    require(!completed && array.status() == raid::RaidStatus::Inconsistent, "write guard cleared after failure");

    Block actual{};
    fixture.first->readBlocks(2, 1, actual.data(), actual.size());
    require(actual == original, "first mirror did not complete before second failed");
    fixture.second->open(fixture.directory.path() / "disk1.img");
    fixture.second->readBlocks(2, 1, actual.data(), actual.size());
    require(actual == Block{}, "test failed to create a stale copy");
    array.markUnavailable("disk0");
    rejects<std::runtime_error>([&] { array.readBlocks(2, 1, actual.data(), actual.size()); },
                               "stale mirror was reused after a partial write");
    rejects<std::runtime_error>([&] { array.writeBlocks(2, 1, actual.data(), actual.size()); },
                               "inconsistent mirror accepted a new write");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view test = argc == 2 ? argv[1] : "";
        if (test == "mirror") {
            testMirror();
        } else if (test == "degraded") {
            testDegraded();
        } else if (test == "partial") {
            testPartialWrite();
        } else {
            throw std::invalid_argument("Unknown RAID 1 test");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
