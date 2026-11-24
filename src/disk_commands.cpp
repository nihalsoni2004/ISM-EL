#include "disk_commands.h"
#include "block_commands.h"

#include "core/config.h"
#include "core/engine.h"
#include "telemetry/telemetry.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using namespace cli;

enum class DiskAction { Create, List, Info, Write, Read, Verify };

struct DiskCommand {
    DiskAction action;
    std::string_view id;
    std::uint64_t number = 0;
    std::string_view text;
};

DiskCommand parseCommand(const std::vector<std::string>& args) {
    if (args.size() == 2 && args[1] == "list") {
        return {DiskAction::List, {}, 0, {}};
    }
    if (args.size() < 3 || !raid::VirtualDiskManager::isValidDiskId(args[2])) {
        throw std::invalid_argument("Expected a disk command and an identifier such as disk0. See --help.");
    }
    const std::string_view action = args[1];
    const std::string_view id = args[2];

    if (action == "create" && (args.size() == 3 || args.size() == 4)) {
        const auto capacity = args.size() == 4 ? parseNumber(args[3], "Capacity")
                                              : raid::config::defaultVirtualDiskSizeBytes / bytesPerMiB;
        if (capacity < 1 || capacity > 1024) {
            throw std::invalid_argument("Capacity must be between 1 and 1024 MiB");
        }
        return {DiskAction::Create, id, capacity * bytesPerMiB, {}};
    }
    if (action == "info" && args.size() == 3) {
        return {DiskAction::Info, id, 0, {}};
    }
    if (action == "write" && args.size() == 5) {
        if (args[4].size() > blockSize) {
            throw std::invalid_argument("Text exceeds the 4096-byte block size");
        }
        return {DiskAction::Write, id, parseNumber(args[3], "LBA"), args[4]};
    }
    if (action == "read" && args.size() == 4) {
        return {DiskAction::Read, id, parseNumber(args[3], "LBA"), {}};
    }
    if (action == "verify" && args.size() == 3) {
        return {DiskAction::Verify, id, 0, {}};
    }
    throw std::invalid_argument("Unknown disk command or incorrect arguments. See --help.");
}

void execute(raid::StorageEngine& engine, const DiskCommand& command) {
    auto& disks = engine.disks();
    const auto id = command.id;
    switch (command.action) {
    case DiskAction::Create: {
        engine.arrays().requireUnownedDisk(id);
        const auto& disk = disks.createDisk(id, command.number);
        std::cout << "Created " << id << ".img (" << capacityLabel(disk.getCapacityBytes())
                  << ", " << disk.getBlockCount() << " blocks).\n";
        break;
    }
    case DiskAction::List: {
        const auto entries = disks.listDisks();
        std::cout << "Virtual Disks\n";
        std::cout << std::left << std::setw(26) << "ID" << std::setw(26) << "Capacity" << "Blocks\n";
        for (const auto& entry : entries) {
            std::cout << std::setw(26) << entry.id << std::setw(26) << capacityLabel(entry.capacityBytes)
                      << entry.blockCount << '\n';
        }
        if (entries.empty()) {
            std::cout << "No virtual disks found.\n";
        }
        break;
    }
    case DiskAction::Info: {
        const auto info = disks.getDiskInfo(id);
        std::cout << "ID: " << info.id << "\nImage: " << info.path.u8string()
                  << "\nCapacity: " << info.capacityBytes << " bytes (" << capacityLabel(info.capacityBytes) << ')'
                  << "\nBlock size: " << info.blockSize << " bytes\nBlocks: " << info.blockCount
                  << "\nState in this process: " << (info.isOpen ? "open" : "closed") << '\n';
        break;
    }
    case DiskAction::Write: {
        const raid::telemetry::Scope scope(raid::telemetry::Origin::Foreground);
        disks.getDiskInfo(id);
        engine.arrays().requireUnownedDisk(id);
        Block block{};
        std::copy(command.text.begin(), command.text.end(), block.begin());
        auto& disk = disks.openDisk(id);
        disk.writeBlocks(command.number, 1, block.data(), block.size());
        disks.closeDisk(id);
        std::cout << "Wrote " << command.text.size() << " UTF-8 bytes to " << id << " LBA "
                  << command.number << " and zero-padded the block.\n";
        break;
    }
    case DiskAction::Read: {
        const raid::telemetry::Scope scope(raid::telemetry::Origin::Foreground);
        Block block{};
        disks.openDisk(id).readBlocks(command.number, 1, block.data(), block.size());
        printBlock(id, command.number, block);
        break;
    }
    case DiskAction::Verify: {
        Block block{};
        auto& disk = disks.openDisk(id);
        const auto last = disk.getBlockCount() - 1;
        disk.readBlocks(0, 1, block.data(), block.size());
        if (last != 0) {
            disk.readBlocks(last, 1, block.data(), block.size());
        }
        std::cout << "Verified " << id << " has a readable, block-aligned image ("
                  << capacityLabel(disk.getCapacityBytes()) << ").\n";
        std::cout << "Read representative LBA 0";
        if (last != 0) {
            std::cout << " and LBA " << last;
        }
        std::cout << " successfully.\n";
        break;
    }
    }
}

}  // namespace

int runDiskCommand(const std::vector<std::string>& args) {
    const auto command = parseCommand(args);
    raid::StorageEngine engine;
    engine.initialize();
    execute(engine, command);
    engine.shutdown();
    return 0;
}
