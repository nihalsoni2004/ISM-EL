#include "rebuild_test_support.h"

#include <future>
#include <iostream>

int main() {
    try {
        using namespace rebuild_test;
        using test::rejects;
        const ArrayFixture fixture(raid::RaidLevel::Raid1, 2);
        addSpare(fixture, "disk90");
        addSpare(fixture, "disk91", memberBlocks - 1);
        addSpare(fixture, "disk92");
        failMember(fixture, 0);
        raid::RebuildManager manager(fixture.root.path(), 4);
        const auto listed = manager.listSpares();
        require(listed.size() == 3 && listed[0].disk.id == "disk90" && listed[0].available &&
                listed[0].state == raid::DiskState::Spare, "registered spares not persisted");
        {
            raid::StorageEngine engine;
            require(engine.initialize(fixture.root.path()), "test initialization failed");
            rejects<std::invalid_argument>([&] { engine.arrays().registerSpare("../disk90"); }, "traversal accepted");
            rejects<std::invalid_argument>([&] { engine.arrays().registerSpare("disk90"); }, "duplicate spare accepted");
            rejects<std::invalid_argument>([&] { engine.arrays().registerSpare("disk0"); }, "owned disk accepted");
            rejects<std::invalid_argument>([&] { engine.arrays().requireUnownedDisk("disk90"); }, "raw write allowed");
            auto& nonzero = engine.disks().createDisk("disk93", memberBlocks * raid::raidBlockSize);
            std::vector<std::uint8_t> block(raid::raidBlockSize, 1);
            nonzero.writeBlocks(0, 1, block.data(), block.size());
            rejects<std::invalid_argument>([&] { engine.arrays().registerSpare("disk93"); }, "nonzero spare accepted");
        }
        fs::create_hard_link(sparePath(fixture, "disk93"), sparePath(fixture, "disk94"));
        {
            raid::StorageEngine engine;
            require(engine.initialize(fixture.root.path()), "test initialization failed");
            rejects<std::invalid_argument>([&] { engine.arrays().registerSpare("disk94"); }, "linked image accepted");
        }
        rejects<std::invalid_argument>([&] { manager.prepare("array0", "disk0", "disk91"); }, "short spare accepted");

        const auto original = sparePath(fixture, "disk92");
        fs::rename(original, fixture.root.path() / "original-spare.img");
        {
            raid::VirtualDisk replacement;
            replacement.create(original, memberBlocks * raid::raidBlockSize);
        }
        require(!manager.listSpares().back().available, "replacement image reported available");
        rejects<std::invalid_argument>([&] { manager.prepare("array0", "disk0", "disk92"); }, "wrong spare identity accepted");

        const auto reserve = [&] {
            try {
                manager.prepare("array0", "disk0", "disk90");
                return true;
            } catch (const std::invalid_argument&) {
                return false;
            }
        };
        auto first = std::async(std::launch::async, reserve);
        auto second = std::async(std::launch::async, reserve);
        const auto firstWon = first.get();
        const auto secondWon = second.get();
        require(firstWon != secondWon && manager.list().size() == 1, "conflicting reservations succeeded");
        require(!manager.listSpares().front().available, "reserved spare still selectable");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
