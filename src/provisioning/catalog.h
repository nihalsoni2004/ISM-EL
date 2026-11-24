#pragma once

#include "provisioning/provisioning_manager.h"

#include <map>

namespace raid::provisioning {

struct Volume {
    std::uint64_t blocks;
    std::map<std::uint64_t, std::uint64_t> mappings;
    std::string fileCatalog;
};

struct Pool {
    std::string id;
    std::string arrayId;
    std::string arrayFingerprint;
    std::vector<std::string> legacyIdentities;
    PoolOptions options;
    std::uint64_t raidBlocks = 0;
    std::uint64_t totalChunks = 0;
    std::map<std::string, Volume, std::less<>> volumes;
    std::string reservedLun;
    std::map<std::uint64_t, std::uint64_t> reserved;
};

inline std::filesystem::path poolPath(const std::filesystem::path& directory, std::string_view arrayId) {
    return directory / (std::string(arrayId) + ".pool");
}

bool validId(std::string_view id, std::string_view prefix) noexcept;
void validateOptions(PoolOptions options);
std::uint64_t logicalChunks(std::uint64_t blocks, std::uint64_t chunkBlocks) noexcept;
std::uint64_t totalProvisioned(const Pool& pool);
bool withinProvisioningLimit(const Pool& pool, std::uint64_t logicalBlocks) noexcept;

class Catalog {
public:
    // The caller holds the RAID catalog lock for this catalog's entire lifetime.
    explicit Catalog(std::filesystem::path directory);
    Pool& pool(std::string_view id);
    Pool& poolForLun(std::string_view id);
    bool ownsArray(std::string_view id) const;
    bool hasPool(std::string_view id) const;
    bool hasLun(std::string_view id) const;
    const std::map<std::string, Pool, std::less<>>& pools() const { return pools_; }
    void save(const Pool& pool, bool create = false) const;

private:
    std::filesystem::path directory_;
    std::map<std::string, Pool, std::less<>> pools_;
    std::map<std::string, std::string, std::less<>> lunPools_;
    std::vector<std::string> arrays_;
};

}  // namespace raid::provisioning
