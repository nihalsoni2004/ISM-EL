#include "provisioning_commands.h"

#include "block_commands.h"
#include "files/file_manager.h"
#include "provisioning/provisioning_manager.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace {

using namespace cli;

std::string capacity(std::uint64_t bytes) {
    if (bytes != 0 && bytes % bytesPerMiB != 0 && bytes % 1024 == 0) {
        return std::to_string(bytes / 1024) + " KiB";
    }
    return capacityLabel(bytes);
}

std::string decimal(long double value) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(2) << value;
    return text.str();
}

void printWatermarks(const raid::PoolInfo& info) {
    std::cout << "Warning threshold: " << info.options.warning << "%\n"
              << "Critical threshold: " << info.options.critical << "%\n";
}

void printPool(const raid::PoolInfo& info) {
    const auto chunkBytes = info.options.chunkBlocks * blockSize;
    const auto usable = info.totalChunks * chunkBytes;
    std::cout << "Pool: " << info.id << "\nBacking RAID: " << info.arrayId
              << "\nRAID health: " << raid::raidHealthName(info.health)
              << "\nRAID logical capacity: " << capacity(info.raidBlocks * blockSize)
              << "\nPhysical pool capacity: " << capacity(usable)
              << "\nPhysical allocated: " << capacity(info.allocatedChunks * chunkBytes)
              << "\nPhysical reserved: " << capacity(info.reservedChunks * chunkBytes)
              << "\nFree physical: " << capacity(info.freeChunks * chunkBytes)
              << "\nLogical provisioned: " << capacity(info.provisionedBlocks * blockSize)
              << "\nChunk size: " << chunkBytes << " bytes"
              << "\nTotal chunks: " << info.totalChunks << "\nAllocated chunks: " << info.allocatedChunks
              << "\nReserved chunks: " << info.reservedChunks << "\nFree chunks: " << info.freeChunks
              << "\nPool utilization: " << decimal(100.0L * (info.allocatedChunks + info.reservedChunks) /
                                                     info.totalChunks) << "%"
              << "\nOverprovisioning ratio: " << decimal(static_cast<long double>(info.provisionedBlocks) *
                                                            blockSize / usable) << "x\n";
    if (info.options.maxProvisioningRatio == 0) {
        std::cout << "Maximum overprovisioning ratio: Unlimited\n";
    } else {
        std::cout << "Maximum overprovisioning ratio: " << info.options.maxProvisioningRatio << "x\n";
    }
    std::cout << "Capacity state: " << raid::capacityStateName(info.capacityState) << '\n';
    printWatermarks(info);
}

void printLun(const raid::LunInfo& info) {
    std::cout << "LUN: " << info.id << "\nPool: " << info.poolId
              << "\nProvisioned capacity: " << capacity(info.blocks * blockSize)
              << "\nAllocated capacity: " << capacity(info.allocatedChunks * info.chunkBlocks * blockSize)
              << "\nLUN mode: " << (info.fileManaged ? "FILE_MANAGED" : "RAW")
              << "\nAllocation chunks: " << info.allocatedChunks
              << "\nBlock size: " << blockSize << " bytes\nChunk size: "
              << info.chunkBlocks * blockSize << " bytes\n";
}

raid::PoolOptions parseOptions(const std::vector<std::string>& args) {
    raid::PoolOptions options;
    bool parsedSize = false;
    bool parsedRatio = false;
    for (std::size_t index = 4; index < args.size(); index += 2) {
        if (index + 1 >= args.size()) {
            throw std::invalid_argument("Pool options require a value");
        }
        if (args[index] == "--chunk-blocks" && !parsedSize) {
            options.chunkBlocks = parseNumber(args[index + 1], "Chunk size in blocks");
            parsedSize = true;
        } else if (args[index] == "--max-ratio" && !parsedRatio) {
            options.maxProvisioningRatio = parseNumber(args[index + 1], "Maximum overprovisioning ratio");
            parsedRatio = true;
        } else {
            throw std::invalid_argument("Unknown or repeated pool option");
        }
    }
    return options;
}

std::uint64_t capacityBytes(std::string_view value) {
    const auto mib = parseNumber(value, "Capacity in MiB");
    if (mib > std::numeric_limits<std::uint64_t>::max() / bytesPerMiB) {
        throw std::overflow_error("LUN capacity in MiB exceeds the supported byte range");
    }
    return mib * bytesPerMiB;
}

std::uint64_t blockCount(std::string_view input) {
    const auto count = parseNumber(input, "Block count");
    if (count == 0 || count > 4096) {
        throw std::invalid_argument("Block count must be between 1 and 4096");
    }
    return count;
}

int readLun(raid::ProvisioningManager& manager, std::string_view id, std::uint64_t lba, std::uint64_t count) {
    std::vector<std::uint8_t> data(count * blockSize);
    const auto first = manager.readBlocks(id, lba, count, data.data(), data.size());
    std::cout << "LUN: " << id << "\n";
    if (count == 1) {
        std::cout << "Logical LBA: " << lba << "\nAllocation state: "
                  << (first.physicalChunk ? "MAPPED" : "UNALLOCATED") << "\n";
    }
    for (std::uint64_t index = 0; index < count; ++index) {
        Block block;
        std::copy_n(data.data() + index * blockSize, block.size(), block.data());
        printBlock(id, lba + index, block);
    }
    return 0;
}

int writeLun(raid::ProvisioningManager& manager, std::string_view id, std::uint64_t lba,
             std::uint64_t count, std::string_view payload) {
    if (payload.size() > blockSize) {
        throw std::invalid_argument("Text exceeds the 4096-byte block size");
    }
    std::vector<std::uint8_t> data(count * blockSize);
    for (std::uint64_t index = 0; index < count; ++index) {
        std::copy(payload.begin(), payload.end(), data.data() + index * blockSize);
    }
    const auto result = manager.writeBlocks(id, lba, count, data.data(), data.size());
    if (count == 1) {
        std::cout << "Written logical LBA: " << lba << '\n';
        if (result.newChunks != 0) {
            std::cout << "Allocated physical chunk: " << result.firstPhysicalChunk << '\n';
        }
    } else {
        std::cout << "Written logical blocks: " << count << "\nStarting LBA: " << lba << '\n';
    }
    std::cout << "New allocation chunks: " << result.newChunks << "\nAllocated capacity: "
              << capacity(result.allocatedChunks * result.chunkBlocks * blockSize) << '\n';
    return 0;
}

}  // namespace

int runPoolCommand(const std::vector<std::string>& args) {
    raid::ProvisioningManager manager(raid::config::projectRoot());
    if (args.size() >= 4 && args[1] == "create") {
        printPool(manager.createPool(args[2], args[3], parseOptions(args)));
    } else if (args.size() == 2 && args[1] == "list") {
        std::cout << "Storage pools\n";
        for (const auto& info : manager.listPools()) {
            std::cout << info.id << ' ' << info.arrayId << ' ' << raid::raidHealthName(info.health)
                      << ' ' << raid::capacityStateName(info.capacityState) << '\n';
        }
    } else if (args.size() == 3 && args[1] == "info") {
        printPool(manager.poolInfo(args[2]));
    } else if (args.size() == 3 && args[1] == "verify") {
        printPool(manager.verifyPool(args[2]));
        std::cout << "Pool metadata, RAID identity, mappings, reservations, and accounting: PASSED\n";
    } else if (args.size() == 3 && args[1] == "watermarks") {
        printWatermarks(manager.poolInfo(args[2]));
    } else if (args.size() == 5 && args[1] == "watermarks") {
        printWatermarks(manager.setWatermarks(args[2], parseNumber(args[3], "Warning percentage"),
                                             parseNumber(args[4], "Critical percentage")));
    } else {
        throw std::invalid_argument("Use pool create, list, info, verify, or watermarks. See --help.");
    }
    return 0;
}

int runLunCommand(const std::vector<std::string>& args) {
    raid::ProvisioningManager manager(raid::config::projectRoot());
    if (args.size() == 5 && args[1] == "create") {
        printLun(manager.createLun(args[2], args[3], capacityBytes(args[4])));
    } else if (args.size() == 7 && args[1] == "create" && args[5] == "--mode" && args[6] == "file") {
        raid::FileManager files(raid::config::projectRoot());
        printLun(files.createLun(args[2], args[3], capacityBytes(args[4])));
    } else if (args.size() == 2 && args[1] == "list") {
        std::cout << "Logical volumes\n";
        for (const auto& info : manager.listLuns()) {
            std::cout << info.id << ' ' << info.poolId << ' ' << capacity(info.blocks * blockSize) << '\n';
        }
    } else if (args.size() == 3 && args[1] == "info") {
        printLun(manager.lunInfo(args[2]));
    } else if (args.size() == 4 && args[1] == "map") {
        const auto lba = parseNumber(args[3], "LBA");
        const auto mapping = manager.map(args[2], lba);
        std::cout << "LUN: " << args[2] << "\nLogical LBA: " << lba << "\nLogical chunk: "
                  << mapping.logicalChunk << "\nOffset: " << mapping.offset << '\n';
        if (mapping.physicalChunk) {
            std::cout << "Physical chunk: " << *mapping.physicalChunk << "\nRAID logical LBA: "
                      << *mapping.raidLba << '\n';
        } else {
            std::cout << "Allocation state: UNALLOCATED\n";
        }
    } else if (args.size() == 4 && args[1] == "read") {
        return readLun(manager, args[2], parseNumber(args[3], "LBA"), 1);
    } else if (args.size() == 5 && args[1] == "read-blocks") {
        return readLun(manager, args[2], parseNumber(args[3], "LBA"), blockCount(args[4]));
    } else if (args.size() == 5 && args[1] == "write") {
        return writeLun(manager, args[2], parseNumber(args[3], "LBA"), 1, args[4]);
    } else if (args.size() == 6 && args[1] == "write-blocks") {
        return writeLun(manager, args[2], parseNumber(args[3], "LBA"), blockCount(args[4]), args[5]);
    } else {
        throw std::invalid_argument("Use lun create, list, info, map, read, read-blocks, write, or write-blocks. See --help.");
    }
    return 0;
}
