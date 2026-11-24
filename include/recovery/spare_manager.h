#pragma once

#include "disk/virtual_disk.h"

#include <optional>
#include <string_view>
#include <vector>

namespace raid {

struct SpareRecord {
    std::string id;
    std::string identity;
    std::uint64_t revision;
    std::uint64_t blocks;
};

// Call under the existing array catalog lock.
class SpareManager {
public:
    explicit SpareManager(std::filesystem::path directory);

    std::optional<SpareRecord> find(std::string_view id) const;
    std::vector<SpareRecord> list() const;
    void registerBlank(std::string_view id, VirtualDisk& disk) const;
    static void validateAvailable(const SpareRecord& record, const VirtualDisk& disk);
    static void validateBlank(VirtualDisk& disk);

private:
    std::filesystem::path directory_;
};

}  // namespace raid
