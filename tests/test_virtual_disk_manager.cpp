#include "core/engine.h"
#include "test_support.h"

#include <array>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using test::rejects;
using test::require;

constexpr std::uint64_t capacity = 1024 * 1024;
using Block = std::array<std::uint8_t, 4096>;

void testLifecycle() {
    test::TempDirectory temp;
    const auto directory = temp.path() / "data" / "virtual_disks";
    Block pattern{};
    pattern.fill(0xA5);

    {
        raid::StorageEngine engine;
        rejects<std::logic_error>([&] { engine.disks(); }, "engine exposes disks before initialization");
        require(engine.initialize(temp.path()) && fs::is_directory(directory), "engine creates storage directory");
        auto& manager = engine.disks();
        require(manager.listDisks().empty(), "initialization does not create images");
        manager.createDisk("disk10", capacity);
        auto& disk = manager.createDisk("disk2", 2 * capacity);
        disk.writeBlocks(7, 1, pattern.data(), pattern.size());
        const auto info = manager.getDiskInfo("disk2");
        require(info.id == "disk2" && info.path == directory / "disk2.img" &&
                info.capacityBytes == 2 * capacity && info.blockSize == 4096 &&
                info.blockCount == 512 && info.isOpen, "disk info matches file and handle");
        const auto disks = manager.listDisks();
        require(disks.size() == 2 && disks[0].id == "disk2" && disks[1].id == "disk10",
                "list includes disks in numeric order");
        rejects<std::invalid_argument>([&] { manager.createDisk("disk2", capacity); }, "duplicate id accepted");
        require(manager.getDiskInfo("disk2").capacityBytes == 2 * capacity, "duplicate preserves disk");
        manager.closeDisk("disk10");
        require(!manager.getDiskInfo("disk10").isOpen, "manager closes disk");
        require(engine.shutdown(), "engine shutdown closes all disks");
        rejects<std::logic_error>([&] { engine.disks(); }, "engine exposes disks after shutdown");
    }

    raid::VirtualDiskManager reopened(directory);
    require(reopened.listDisks().size() == 2 && !reopened.getDiskInfo("disk2").isOpen,
            "new manager discovers persisted closed images");
    Block recovered{};
    reopened.openDisk("disk2").readBlocks(7, 1, recovered.data(), recovered.size());
    require(recovered == pattern, "dirty block persists through engine shutdown");
    reopened.closeAll();
}

void testIsolation() {
    test::TempDirectory temp;
    const auto directory = temp.path() / "store";
    const auto outside = temp.path() / "outside";
    raid::VirtualDiskManager manager(directory);
    raid::VirtualDiskManager other(outside);
    other.createDisk("disk7", capacity);
    other.closeAll();

    for (const auto invalid : {"", "disk", "DISK1", "disk01", "disk-1", "disk+1",
                               "disk18446744073709551616", "../disk7", "disk7/../disk8",
                               "C:\\disk7", "disk7.img", "disk7:other"}) {
        rejects<std::invalid_argument>([&] { manager.createDisk(invalid, capacity); }, "invalid id accepted");
        rejects<std::invalid_argument>([&] { manager.openDisk(invalid); }, "invalid id opened");
    }
    rejects<fs::filesystem_error>([&] { manager.openDisk("disk7"); }, "outside disk exposed");
    rejects<fs::filesystem_error>([&] { manager.getDiskInfo("disk7"); }, "outside info exposed");

    std::ofstream{directory / "ignore.txt"}.put('x');
    std::ofstream{directory / "other.img"}.put('x');
    std::ofstream{directory / "disk3.img"}.put('x');
    fs::create_directory(directory / "disk4.img");
    rejects<std::invalid_argument>([&] { manager.getDiskInfo("disk3"); }, "malformed image accepted");
    rejects<std::invalid_argument>([&] { manager.getDiskInfo("disk4"); }, "directory image accepted");

    fs::create_hard_link(outside / "disk7.img", directory / "disk5.img");
    rejects<std::invalid_argument>([&] { manager.openDisk("disk5"); }, "hard-linked image accepted");
    require(manager.listDisks().empty(), "list excludes unsupported names, shapes, and links");
    require(fs::file_size(outside / "disk7.img") == capacity, "outside image was preserved");

    const auto blockingFile = temp.path() / "blocked";
    std::ofstream{blockingFile}.put('x');
    rejects<std::invalid_argument>([&] { raid::VirtualDiskManager blocked(blockingFile / "store"); },
                                   "manager accepted file as directory");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view test = argc == 2 ? argv[1] : "";
        if (test == "lifecycle") {
            testLifecycle();
        } else if (test == "isolation") {
            testIsolation();
        } else {
            throw std::invalid_argument("Unknown virtual disk manager test");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
