#include "disk/virtual_disk_manager.h"

#include "core/config.h"
#include "image_file.h"

#include <algorithm>
#include <charconv>
#include <exception>

namespace raid {

VirtualDiskManager::VirtualDiskManager(std::filesystem::path directory)
    : directory_(std::move(directory)) {
    disk_file::prepareDirectory(directory_, true);
}

bool VirtualDiskManager::isValidDiskId(std::string_view id) noexcept {
    if (id.size() < 5 || id.substr(0, 4) != "disk" || (id.size() > 5 && id[4] == '0')) {
        return false;
    }

    std::uint64_t number = 0;
    const auto end = id.data() + id.size();
    const auto parsed = std::from_chars(id.data() + 4, end, number);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

std::filesystem::path VirtualDiskManager::imagePath(std::string_view id) const {
    if (!isValidDiskId(id)) {
        throw std::invalid_argument("Invalid disk identifier. Use disk0, disk1, and so on.");
    }
    return directory_ / (std::string(id) + ".img");
}

VirtualDisk& VirtualDiskManager::createDisk(std::string_view id, std::uint64_t capacityBytes) {
    const auto path = imagePath(id);
    auto disk = std::make_shared<VirtualDisk>();
    disk->create(path, capacityBytes);
    return *disks_.insert_or_assign(std::string(id), std::move(disk)).first->second;
}

VirtualDisk& VirtualDiskManager::openDisk(std::string_view id) {
    const auto path = imagePath(id);
    const auto existing = disks_.find(id);
    if (existing != disks_.end()) {
        if (!existing->second->isOpen()) {
            existing->second->open(path);
        }
        return *existing->second;
    }

    auto disk = std::make_shared<VirtualDisk>();
    disk->open(path);
    return *disks_.emplace(id, std::move(disk)).first->second;
}

std::shared_ptr<VirtualDisk> VirtualDiskManager::leaseDisk(std::string_view id) {
    openDisk(id);
    return disks_.find(id)->second;
}

void VirtualDiskManager::closeDisk(std::string_view id) {
    if (!isValidDiskId(id)) {
        throw std::invalid_argument("Invalid disk identifier. Use disk0, disk1, and so on.");
    }
    const auto existing = disks_.find(id);
    if (existing != disks_.end()) {
        if (existing->second.use_count() > 1) {
            throw std::logic_error("Cannot close a disk while a RAID controller is using it");
        }
        auto disk = std::move(existing->second);
        disks_.erase(existing);
        disk->close();
    }
}

void VirtualDiskManager::closeAll() {
    std::exception_ptr failure;
    for (const auto& entry : disks_) {
        try {
            entry.second->close();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
    }
    disks_.clear();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

VirtualDiskInfo VirtualDiskManager::getDiskInfo(std::string_view id) const {
    const auto path = imagePath(id);
    const auto capacity = disk_file::inspect(path);
    constexpr auto blockSize = config::defaultLogicalBlockSizeBytes;
    const auto existing = disks_.find(id);
    return {std::string(id), path, capacity, blockSize, capacity / blockSize,
            existing != disks_.end() && existing->second->isOpen()};
}

std::vector<VirtualDiskInfo> VirtualDiskManager::listDisks() const {
    std::vector<VirtualDiskInfo> result;
    for (const auto& path : disk_file::listEntries(directory_)) {
        if (path.extension() != L".img") {
            continue;
        }
        const auto id = path.stem().u8string();
        if (!isValidDiskId(id)) {
            continue;
        }
        try {
            result.push_back(getDiskInfo(id));
        } catch (const disk_file::InvalidImage&) {
            // Other files in the data directory do not represent supported disks.
        } catch (const std::filesystem::filesystem_error& error) {
            if (error.code() != std::errc::no_such_file_or_directory) {
                throw;
            }
            // An entry disappeared between enumeration and inspection.
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.id.size() != rhs.id.size()) {
            return lhs.id.size() < rhs.id.size();
        }
        return lhs.id < rhs.id;
    });
    return result;
}

}  // namespace raid
