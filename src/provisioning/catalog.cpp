#include "provisioning/catalog.h"

#include "raid/raid_array_manager.h"
#include "recovery/record.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>

namespace raid::provisioning {
namespace {

constexpr std::uint32_t metadataLimit = 8 * 1024 * 1024;
constexpr auto maxBlocks = std::numeric_limits<std::uint64_t>::max() / raidBlockSize;

std::uint64_t number(std::istream& input, std::string_view key) {
    return record::number(record::field(input, key));
}

void validateBackend(const Pool& pool) {
    if (!validId(pool.id, "pool") || !RaidArrayManager::isValidArrayId(pool.arrayId) ||
        pool.arrayFingerprint.size() < 66 || pool.arrayFingerprint[64] != ':' ||
        !record::identity(std::string_view(pool.arrayFingerprint).substr(0, 64))) {
        throw std::invalid_argument("Invalid pool ID or backing RAID identity");
    }
    record::number(std::string_view(pool.arrayFingerprint).substr(65));
    for (const auto& identity : pool.legacyIdentities) {
        if (!record::identity(identity)) {
            throw std::invalid_argument("Invalid legacy RAID member identity");
        }
    }
    validateOptions(pool.options);
    if (pool.raidBlocks > maxBlocks || pool.totalChunks == 0 ||
        pool.totalChunks != pool.raidBlocks / pool.options.chunkBlocks) {
        throw std::invalid_argument("Invalid pool geometry");
    }
}

void loadMappings(std::istream& input, Pool& pool, std::string_view field,
                  std::uint64_t count, std::set<std::uint64_t>& used, bool reserved) {
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto fields = record::split(record::field(input, field));
        if (fields.size() != 3) {
            throw std::invalid_argument("Invalid provisioning mapping fields");
        }
        const auto lun = pool.volumes.find(fields[0]);
        const auto logical = record::number(fields[1]);
        const auto physical = record::number(fields[2]);
        if (lun == pool.volumes.end() || logical >= logicalChunks(lun->second.blocks, pool.options.chunkBlocks) ||
            physical >= pool.totalChunks || !used.insert(physical).second || lun->second.mappings.count(logical)) {
            throw std::invalid_argument("Conflicting or out-of-range provisioning mapping");
        }
        if (reserved) {
            if (pool.reservedLun.empty()) {
                pool.reservedLun = fields[0];
            }
            if (pool.reservedLun != fields[0] || !pool.reserved.emplace(logical, physical).second) {
                throw std::invalid_argument("Conflicting allocation reservation");
            }
        } else {
            lun->second.mappings.emplace(logical, physical);
        }
    }
}

Pool parse(const std::string& text, std::string_view fileArrayId) {
    std::istringstream input(text);
    if (text.empty() || text.back() != '\n') {
        throw std::invalid_argument("Truncated or unsupported provisioning catalog");
    }
    const auto schema = number(input, "schema_version");
    if (schema != 1 && schema != 2) {
        throw std::invalid_argument("Unsupported provisioning catalog version");
    }
    Pool pool;
    pool.id = record::field(input, "pool_id");
    pool.arrayId = record::field(input, "array_id");
    if (pool.arrayId != fileArrayId || number(input, "block_size") != raidBlockSize) {
        throw std::invalid_argument("Pool record filename or block size mismatch");
    }
    pool.arrayFingerprint = record::field(input, "array_fingerprint");
    pool.legacyIdentities = record::split(record::field(input, "legacy_identities"));
    pool.options.chunkBlocks = number(input, "chunk_blocks");
    pool.raidBlocks = number(input, "raid_blocks");
    pool.totalChunks = number(input, "total_chunks");
    pool.options.warning = number(input, "warning");
    pool.options.critical = number(input, "critical");
    pool.options.maxProvisioningRatio = number(input, "max_ratio");
    validateBackend(pool);
    const auto lunCount = number(input, "lun_count");
    for (std::uint64_t index = 0; index < lunCount; ++index) {
        const auto fields = record::split(record::field(input, "lun"));
        if (fields.size() != (schema == 1 ? 3U : 4U) || !validId(fields[0], "lun")) {
            throw std::invalid_argument("Invalid LUN record");
        }
        const auto blocks = record::number(fields[1]);
        const auto authorization = schema == 2 && fields[3] != "-" ? fields[3] : std::string{};
        if (!authorization.empty() && !record::identity(authorization)) {
            throw std::invalid_argument("Invalid file-managed LUN catalog identity");
        }
        if (blocks == 0 || blocks > maxBlocks ||
            record::number(fields[2]) != logicalChunks(blocks, pool.options.chunkBlocks) ||
            !pool.volumes.emplace(fields[0], Volume{blocks, {}, authorization}).second) {
            throw std::invalid_argument("Invalid or duplicate LUN geometry");
        }
    }
    if (!withinProvisioningLimit(pool, totalProvisioned(pool))) {
        throw std::invalid_argument("Logical provisioning exceeds pool limit");
    }
    const auto allocated = number(input, "allocated_chunks");
    const auto reserved = number(input, "reserved_chunks");
    const auto free = number(input, "free_chunks");
    if (allocated > pool.totalChunks || reserved > pool.totalChunks - allocated ||
        free != pool.totalChunks - allocated - reserved) {
        throw std::invalid_argument("Invalid provisioning chunk accounting");
    }
    std::set<std::uint64_t> used;
    loadMappings(input, pool, "mapping", allocated, used, false);
    loadMappings(input, pool, "reservation", reserved, used, true);
    if (number(input, "end") != 1) {
        throw std::invalid_argument("Invalid end of provisioning record");
    }
    record::requireEnd(input);
    return pool;
}

std::string serialize(const Pool& pool) {
    std::ostringstream text;
    const auto managed = std::any_of(pool.volumes.begin(), pool.volumes.end(), [](const auto& entry) {
        return !entry.second.fileCatalog.empty();
    });
    text << "schema_version=" << (managed ? 2 : 1) << "\npool_id=" << pool.id << "\narray_id=" << pool.arrayId
         << "\nblock_size=" << raidBlockSize << "\narray_fingerprint=" << pool.arrayFingerprint
         << "\nlegacy_identities=" << record::join(pool.legacyIdentities)
         << "\nchunk_blocks=" << pool.options.chunkBlocks << "\nraid_blocks=" << pool.raidBlocks
         << "\ntotal_chunks=" << pool.totalChunks << "\nwarning=" << pool.options.warning
         << "\ncritical=" << pool.options.critical << "\nmax_ratio=" << pool.options.maxProvisioningRatio
         << "\nlun_count=" << pool.volumes.size() << '\n';
    std::uint64_t allocated = 0;
    for (const auto& volume : pool.volumes) {
        text << "lun=" << volume.first << ',' << volume.second.blocks << ','
             << logicalChunks(volume.second.blocks, pool.options.chunkBlocks);
        if (managed) {
            text << ',' << (volume.second.fileCatalog.empty() ? "-" : volume.second.fileCatalog);
        }
        text << '\n';
        allocated += volume.second.mappings.size();
    }
    text << "allocated_chunks=" << allocated << "\nreserved_chunks=" << pool.reserved.size()
         << "\nfree_chunks=" << pool.totalChunks - allocated - pool.reserved.size() << '\n';
    for (const auto& volume : pool.volumes) {
        for (const auto& mapping : volume.second.mappings) {
            text << "mapping=" << volume.first << ',' << mapping.first << ',' << mapping.second << '\n';
        }
    }
    for (const auto& mapping : pool.reserved) {
        text << "reservation=" << pool.reservedLun << ',' << mapping.first << ',' << mapping.second << '\n';
    }
    text << "end=1\n";
    return text.str();
}

}  // namespace

bool validId(std::string_view id, std::string_view prefix) noexcept {
    if (id.substr(0, prefix.size()) != prefix || id.size() <= prefix.size()) {
        return false;
    }
    id.remove_prefix(prefix.size());
    if (id.size() > 1 && id.front() == '0') {
        return false;
    }
    std::uint64_t value = 0;
    const auto end = id.data() + id.size();
    const auto result = std::from_chars(id.data(), end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

void validateOptions(PoolOptions options) {
    if (options.chunkBlocks == 0 || options.chunkBlocks > 1024) {
        throw std::invalid_argument("Pool chunk size must be between 1 and 1024 logical blocks");
    }
    if (options.warning == 0 || options.warning >= options.critical || options.critical > 100) {
        throw std::invalid_argument("Pool watermarks require 1 <= warning < critical <= 100");
    }
}

std::uint64_t logicalChunks(std::uint64_t blocks, std::uint64_t chunkBlocks) noexcept {
    return blocks / chunkBlocks + (blocks % chunkBlocks != 0);
}

std::uint64_t totalProvisioned(const Pool& pool) {
    std::uint64_t total = 0;
    for (const auto& volume : pool.volumes) {
        if (volume.second.blocks > maxBlocks - total) {
            throw std::overflow_error("Total provisioned capacity exceeds the supported byte range");
        }
        total += volume.second.blocks;
    }
    return total;
}

bool withinProvisioningLimit(const Pool& pool, std::uint64_t logicalBlocks) noexcept {
    const auto ratio = pool.options.maxProvisioningRatio;
    return ratio == 0 || logicalChunks(logicalBlocks, pool.totalChunks * pool.options.chunkBlocks) <= ratio;
}

Catalog::Catalog(std::filesystem::path directory) : directory_(std::move(directory)) {
    for (const auto& path : record::entries(directory_)) {
        if (!record::hasExtension(path, L".pool")) {
            continue;
        }
        const auto contents = disk_file::readText(path, metadataLimit);
        if (!contents) {
            throw std::runtime_error("Pool metadata disappeared during catalog inspection");
        }
        auto pool = parse(*contents, path.stem().u8string());
        const auto id = pool.id;
        if (ownsArray(pool.arrayId) || hasPool(id)) {
            throw std::invalid_argument("Duplicate pool ID or RAID ownership");
        }
        for (const auto& volume : pool.volumes) {
            if (!lunPools_.emplace(volume.first, id).second) {
                throw std::invalid_argument("Duplicate LUN ID across pools: " + volume.first);
            }
        }
        arrays_.push_back(pool.arrayId);
        pools_.emplace(id, std::move(pool));
    }
}

Pool& Catalog::pool(std::string_view id) {
    const auto found = pools_.find(id);
    if (found == pools_.end()) {
        throw std::invalid_argument("Storage pool does not exist: " + std::string(id));
    }
    return found->second;
}

Pool& Catalog::poolForLun(std::string_view id) {
    const auto found = lunPools_.find(id);
    if (found == lunPools_.end()) {
        throw std::invalid_argument("LUN does not exist: " + std::string(id));
    }
    return pool(found->second);
}

bool Catalog::ownsArray(std::string_view id) const {
    return std::find(arrays_.begin(), arrays_.end(), id) != arrays_.end();
}

bool Catalog::hasPool(std::string_view id) const {
    return pools_.find(id) != pools_.end();
}

bool Catalog::hasLun(std::string_view id) const {
    return lunPools_.find(id) != lunPools_.end();
}

void Catalog::save(const Pool& pool, bool create) const {
    const auto text = serialize(pool);
    if (text.size() > metadataLimit) {
        throw std::runtime_error("Provisioning metadata exceeds the 8 MiB pool record limit");
    }
    const auto path = poolPath(directory_, pool.arrayId);
    if (create) {
        disk_file::prepareDirectory(directory_, true);
        disk_file::publishText(path, text, metadataLimit);
    } else {
        disk_file::replaceText(path, text, metadataLimit);
    }
}

}  // namespace raid::provisioning
