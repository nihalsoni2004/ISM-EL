#include "recovery/spare_manager.h"

#include "core/config.h"
#include "disk/virtual_disk_manager.h"
#include "record.h"

#include <algorithm>

namespace raid {

SpareManager::SpareManager(std::filesystem::path directory) : directory_(std::move(directory)) {}

std::optional<SpareRecord> SpareManager::find(std::string_view id) const {
    if (!VirtualDiskManager::isValidDiskId(id)) {
        throw std::invalid_argument("Invalid spare disk identifier");
    }
    const auto text = record::read(directory_ / (std::string(id) + ".spare"));
    if (!text) {
        return std::nullopt;
    }
    std::istringstream input(*text);
    if (record::field(input, "schema_version") != "1" || record::field(input, "disk_id") != id) {
        throw std::invalid_argument("Unsupported spare record schema or disk ID");
    }
    auto identity = record::field(input, "identity");
    const auto revision = record::number(record::field(input, "revision"));
    const auto blocks = record::number(record::field(input, "blocks"));
    record::requireEnd(input);
    if (!record::identity(identity) || blocks == 0 || blocks > INT64_MAX / config::defaultLogicalBlockSizeBytes) {
        throw std::invalid_argument("Invalid spare record geometry or identity");
    }
    return SpareRecord{std::string(id), std::move(identity), revision, blocks};
}

std::vector<SpareRecord> SpareManager::list() const {
    std::vector<SpareRecord> spares;
    for (const auto& entry : record::entries(directory_)) {
        if (record::hasExtension(entry, L".spare")) {
            const auto spare = find(entry.stem().u8string());
            if (!spare) {
                throw std::runtime_error("Spare record disappeared during catalog inspection");
            }
            spares.push_back(*spare);
        }
    }
    std::sort(spares.begin(), spares.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return spares;
}

void SpareManager::validateBlank(VirtualDisk& disk) {
    constexpr std::uint64_t chunkBlocks = 64;
    const auto blockSize = disk.getBlockSize();
    std::vector<std::uint8_t> buffer(chunkBlocks * blockSize);
    for (std::uint64_t lba = 0; lba < disk.getBlockCount();) {
        const auto count = std::min(chunkBlocks, disk.getBlockCount() - lba);
        const auto bytes = static_cast<std::size_t>(count * blockSize);
        disk.readBlocks(lba, count, buffer.data(), bytes);
        if (std::any_of(buffer.data(), buffer.data() + bytes, [](auto byte) { return byte != 0; })) {
            throw std::invalid_argument("Spare image contains nonzero data");
        }
        lba += count;
    }
}

void SpareManager::validateAvailable(const SpareRecord& record, const VirtualDisk& disk) {
    if (disk.imageIdentity() != record.identity || disk.imageRevision() != record.revision ||
        disk.getBlockCount() != record.blocks) {
        throw std::invalid_argument("Spare image identity, revision, or geometry changed: " + record.id);
    }
}

void SpareManager::registerBlank(std::string_view id, VirtualDisk& disk) const {
    if (find(id)) {
        throw std::invalid_argument("Disk is already registered as a spare: " + std::string(id));
    }
    validateBlank(disk);
    std::ostringstream text;
    text << "schema_version=1\ndisk_id=" << id << "\nidentity=" << disk.imageIdentity()
         << "\nrevision=" << disk.imageRevision() << "\nblocks=" << disk.getBlockCount() << '\n';
    disk_file::prepareDirectory(directory_, true);
    disk_file::publishText(directory_ / (std::string(id) + ".spare"), text.str());
}

}  // namespace raid
