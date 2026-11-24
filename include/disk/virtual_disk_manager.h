#pragma once

#include "disk/virtual_disk.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace raid {

struct VirtualDiskInfo {
    std::string id;
    std::filesystem::path path;
    std::uint64_t capacityBytes;
    std::uint64_t blockSize;
    std::uint64_t blockCount;
    bool isOpen;
};

class VirtualDiskManager {
public:
    explicit VirtualDiskManager(std::filesystem::path directory);
    VirtualDiskManager(const VirtualDiskManager&) = delete;
    VirtualDiskManager& operator=(const VirtualDiskManager&) = delete;

    static bool isValidDiskId(std::string_view id) noexcept;

    VirtualDisk& createDisk(std::string_view id, std::uint64_t capacityBytes);
    VirtualDisk& openDisk(std::string_view id);
    void closeDisk(std::string_view id);
    void closeAll();

    VirtualDiskInfo getDiskInfo(std::string_view id) const;
    std::vector<VirtualDiskInfo> listDisks() const;

private:
    friend class RaidArrayManager;
    std::shared_ptr<VirtualDisk> leaseDisk(std::string_view id);
    std::filesystem::path imagePath(std::string_view id) const;

    std::filesystem::path directory_;
    std::map<std::string, std::shared_ptr<VirtualDisk>, std::less<>> disks_;
};

}  // namespace raid
