#include "raid/raid_array_manager.h"

#include "raid/raid0.h"
#include "raid/raid1.h"
#include "raid/raid5.h"
#include "raid/raid6.h"
#include "disk/image_file.h"
#include "provisioning/catalog.h"

#include <algorithm>
#include <charconv>
#include <set>
#include <sstream>
#include <stdexcept>

namespace raid {
namespace {

namespace fs = std::filesystem;
constexpr std::string_view mirrorMarker = "schema_version=1\nmirror_write=pending\n";
constexpr std::string_view parityMarker = "schema_version=1\nparity_write=pending\n";

std::uint64_t number(std::string_view value, std::string_view field) {
    std::uint64_t result = 0;
    const auto end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        throw std::invalid_argument("Invalid RAID metadata field " + std::string(field));
    }
    return result;
}

std::string field(std::istream& input, const std::string& key) {
    std::string line;
    const auto prefix = key + '=';
    if (!std::getline(input, line) || line.compare(0, prefix.size(), prefix) != 0) {
        throw std::invalid_argument("Expected RAID metadata field " + key);
    }
    if (line.back() == '\r') {
        line.pop_back();
    }
    return line.substr(prefix.size());
}

void validateMembers(const std::vector<std::string>& members) {
    std::set<std::string_view> unique;
    for (const auto& id : members) {
        if (!VirtualDiskManager::isValidDiskId(id)) {
            throw std::invalid_argument("Invalid RAID member disk identifier: " + id);
        }
        if (!unique.insert(id).second) {
            throw std::invalid_argument("Duplicate RAID member: " + id);
        }
    }
}

RaidConfiguration parse(const std::string& text, std::string_view filenameId) {
    std::istringstream input(text);
    const auto version = number(field(input, "schema_version"), "schema_version");
    if (version != raidSchemaVersion) {
        throw std::invalid_argument("Unsupported RAID metadata schema version");
    }
    auto id = field(input, "array_id");
    if (id != filenameId || !RaidArrayManager::isValidArrayId(id)) {
        throw std::invalid_argument("RAID array identifier does not match its metadata filename");
    }
    const auto levelValue = number(field(input, "raid_level"), "raid_level");
    if (levelValue != 0 && levelValue != 1 && levelValue != 5 && levelValue != 6) {
        throw std::invalid_argument("Unsupported RAID level in metadata");
    }
    const auto level = static_cast<RaidLevel>(levelValue);
    auto memberList = field(input, "members");
    std::vector<std::string> members;
    std::string_view remaining = memberList;
    for (;;) {
        const auto separator = remaining.find(',');
        members.emplace_back(remaining.substr(0, separator));
        if (separator == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(separator + 1);
    }
    validateMembers(members);

    const auto blockSize = number(field(input, "block_size"), "block_size");
    const auto stripe = number(field(input, "stripe_unit_blocks"), "stripe_unit_blocks");
    const auto capacity = number(field(input, "capacity_blocks"), "capacity_blocks");
    std::string layout;
    const auto expectedLayout = parityLayout(level);
    if (!expectedLayout.empty()) {
        layout = field(input, "layout");
        if (layout != expectedLayout) {
            throw std::invalid_argument("Unsupported RAID " + std::to_string(levelValue) + " parity layout: " + layout);
        }
    }
    std::string extra;
    std::vector<std::string> identities;
    if (std::getline(input, extra)) {
        constexpr std::string_view prefix = "member_identities=";
        if (!extra.empty() && extra.back() == '\r') {
            extra.pop_back();
        }
        if (extra.compare(0, prefix.size(), prefix) != 0) {
            throw std::invalid_argument("Unexpected RAID metadata field");
        }
        std::string_view remainingIds(extra);
        remainingIds.remove_prefix(prefix.size());
        for (;;) {
            const auto comma = remainingIds.find(',');
            const auto identity = remainingIds.substr(0, comma);
            if (identity.size() != 64 || !std::all_of(identity.begin(), identity.end(), [](char ch) {
                    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
                })) {
                throw std::invalid_argument("Invalid RAID member image identity");
            }
            identities.emplace_back(identity);
            if (comma == std::string_view::npos) {
                break;
            }
            remainingIds.remove_prefix(comma + 1);
        }
        if (identities.size() != members.size() || std::getline(input, extra)) {
            throw std::invalid_argument("Incorrect RAID member image identity count or extra fields");
        }
    }
    if (blockSize != raidBlockSize) {
        throw std::invalid_argument("Unexpected RAID metadata fields or block size");
    }
    const auto memberCount = members.size();
    validateRaidLayout(level, memberCount, stripe);
    const auto memberBlocks = capacity / dataUnitCount(level, memberCount);
    if (calculateRaidCapacity(level, memberCount, memberBlocks, stripe) != capacity) {
        throw std::invalid_argument("Invalid RAID metadata capacity");
    }
    return {std::move(id), level, std::move(members), stripe, capacity, blockSize, version,
            std::move(layout), std::move(identities)};
}

std::string serialize(const RaidConfiguration& configuration) {
    std::ostringstream output;
    output << "schema_version=" << configuration.schemaVersion
           << "\narray_id=" << configuration.id
           << "\nraid_level=" << static_cast<int>(configuration.level)
           << "\nmembers=";
    const char* separator = "";
    for (const auto& member : configuration.members) {
        output << separator << member;
        separator = ",";
    }
    output << "\nblock_size=" << configuration.blockSize
           << "\nstripe_unit_blocks=" << configuration.stripeUnitBlocks
           << "\ncapacity_blocks=" << configuration.capacityBlocks << '\n';
    if (!configuration.layout.empty()) {
        output << "layout=" << configuration.layout << '\n';
    }
    if (!configuration.memberIdentities.empty()) {
        output << "member_identities=";
        for (std::size_t index = 0; index < configuration.memberIdentities.size(); ++index) {
            output << (index == 0 ? "" : ",") << configuration.memberIdentities[index];
        }
        output << '\n';
    }
    return output.str();
}

bool owns(const RaidConfiguration& array, std::string_view disk) {
    return std::find(array.members.begin(), array.members.end(), disk) != array.members.end() ||
           std::find(array.originalMembers.begin(), array.originalMembers.end(), disk) != array.originalMembers.end();
}

void requireBlank(VirtualDisk& disk, std::string_view id) {
    constexpr std::uint64_t chunkBlocks = 64;
    std::vector<std::uint8_t> data(chunkBlocks * raidBlockSize);
    for (std::uint64_t lba = 0; lba < disk.getBlockCount();) {
        const auto blocks = std::min(chunkBlocks, disk.getBlockCount() - lba);
        const auto bytes = static_cast<std::size_t>(blocks * raidBlockSize);
        disk.readBlocks(lba, blocks, data.data(), bytes);
        if (std::any_of(data.data(), data.data() + bytes, [](auto byte) { return byte != 0; })) {
            throw std::invalid_argument("RAID member contains nonzero data: " + std::string(id));
        }
        lba += blocks;
    }
}

bool isMetadataFile(const fs::path& path) {
    auto extension = path.extension().native();
    for (auto& character : extension) {
        if (character >= L'A' && character <= L'Z') {
            character += L'a' - L'A';
        }
    }
    return extension == L".raid";
}

}  // namespace

RaidArrayManager::RaidArrayManager(VirtualDiskManager& disks, fs::path directory)
    : disks_(disks), directory_(std::move(directory)), failures_(directory_), spares_(directory_.parent_path() / "spares") {}

bool RaidArrayManager::isValidArrayId(std::string_view id) noexcept {
    constexpr std::string_view prefix = "array";
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

void RaidArrayManager::lockCatalog() {
    if (!catalogLock_) {
        catalogLock_ = disk_file::lockCatalog(directory_ / ".catalog.lock");
    }
}

RaidArrayManager::Catalog RaidArrayManager::loadCatalog() {
    lockCatalog();
    Catalog catalog;
    std::set<std::string> claimed;
    for (const auto& path : disk_file::listEntries(directory_)) {
        if (!isMetadataFile(path)) {
            continue;
        }
        try {
            const auto id = path.stem().u8string();
            const auto text = disk_file::readText(path);
            if (!text) {
                throw std::runtime_error("RAID metadata disappeared during discovery");
            }
            auto configuration = parse(*text, id);
            const auto state = failures_.load(configuration);
            if (!state.activeMembers.empty()) {
                configuration.originalMembers = std::move(configuration.members);
                configuration.members = state.activeMembers;
                configuration.memberIdentities = state.activeIdentities;
                configuration.completedRebuilds = state.completedJobs;
            }
            std::set<std::string> owned(configuration.members.begin(), configuration.members.end());
            owned.insert(configuration.originalMembers.begin(), configuration.originalMembers.end());
            for (const auto& disk : owned) {
                if (!claimed.insert(disk).second) {
                    throw std::invalid_argument("Disk appears in more than one RAID array: " + disk);
                }
            }
            if (!catalog.emplace(id, std::move(configuration)).second) {
                throw std::invalid_argument("Duplicate RAID metadata for " + id);
            }
        } catch (const std::exception& error) {
            throw std::invalid_argument("RAID catalog entry " + path.filename().u8string() + ": " + error.what());
        }
    }
    return catalog;
}

RaidArrayManager::Members RaidArrayManager::openMembers(const std::vector<std::string>& ids) {
    Members members;
    members.reserve(ids.size());
    for (const auto& id : ids) {
        try {
            members.push_back(disks_.leaseDisk(id));
        } catch (const std::exception& error) {
            throw std::runtime_error("Cannot open RAID member " + id + ": " + error.what());
        }
    }
    return members;
}

RaidConfiguration RaidArrayManager::findArray(std::string_view id) {
    if (!isValidArrayId(id)) {
        throw std::invalid_argument("Invalid RAID array identifier. Use array0, array1, and so on.");
    }
    auto catalog = loadCatalog();
    const auto found = catalog.find(id);
    if (found == catalog.end()) {
        throw std::invalid_argument("RAID array does not exist: " + std::string(id));
    }
    return std::move(found->second);
}

RaidArrayManager::ResolvedMembers RaidArrayManager::resolveMembers(const RaidConfiguration& configuration,
                                                                   bool acquireHandles) {
    auto state = failures_.load(configuration);
    ResolvedMembers resolved;
    resolved.status.reserve(configuration.members.size());
    resolved.disks.reserve(configuration.members.size());
    const auto expectedBlocks = configuration.capacityBlocks /
        dataUnitCount(configuration.level, configuration.members.size());
    const auto expectedBytes = expectedBlocks * configuration.blockSize;
    for (std::size_t index = 0; index < configuration.members.size(); ++index) {
        const auto& id = configuration.members[index];
        const auto path = disks_.imagePath(id);
        std::shared_ptr<VirtualDisk> disk;
        if (state.members[index].state == DiskState::Online) {
            auto cause = FailureCause::None;
            const bool checkIdentity = !configuration.memberIdentities.empty() &&
                                       configuration.memberIdentities[index] != "-";
            std::string identity;
            std::uint64_t bytes = 0;
            try {
                if (acquireHandles) {
                    disk = disks_.leaseDisk(id);
                    bytes = disk->getCapacityBytes();
                    if (checkIdentity) {
                        identity = disk->imageIdentity();
                    }
                } else {
                    bytes = disk_file::inspect(path, checkIdentity ? &identity : nullptr);
                }
            } catch (const fs::filesystem_error& error) {
                if (error.code() != std::errc::no_such_file_or_directory) {
                    throw std::runtime_error("Cannot open RAID member " + id + ": " + error.what());
                }
                cause = FailureCause::MissingImage;
            } catch (const std::exception& error) {
                throw std::runtime_error("Cannot open RAID member " + id + ": " + error.what());
            }

            if (cause == FailureCause::None) {
                if (checkIdentity && identity != configuration.memberIdentities[index]) {
                    cause = FailureCause::ImageChanged;
                } else if (bytes != expectedBytes) {
                    throw std::invalid_argument("RAID member " + id + " differs from stored geometry");
                }
            }
            if (cause != FailureCause::None) {
                failures_.failMember(configuration, state, index, cause);
                const auto cached = arrays_.find(configuration.id);
                if (cached != arrays_.end()) {
                    cached->second->markUnavailable(id);
                }
                if (disk) {
                    // Release the suspect handle without ever issuing RAID block I/O.
                    disk.reset();
                    disks_.closeDisk(id);
                }
            }
        }
        const bool online = state.members[index].state == DiskState::Online;
        // On controller open, failed members are deliberately never opened or probed.
        const bool present = online || (!acquireHandles && disk_file::imagePresent(path));
        resolved.status.push_back({id, state.members[index], present, online});
        if (acquireHandles) {
            resolved.disks.push_back(std::move(disk));
        }
    }
    return resolved;
}

bool RaidArrayManager::hasPendingWrite(const RaidConfiguration& configuration) const {
    const auto marker = directory_ / (configuration.id + ".pending");
    const auto contents = configuration.level == RaidLevel::Raid1 ? mirrorMarker : parityMarker;
    const auto previousWrite = disk_file::readText(marker);
    if (previousWrite && *previousWrite != contents) {
        throw std::invalid_argument("Malformed RAID write marker: " + configuration.id);
    }
    return previousWrite.has_value();
}

bool RaidArrayManager::poolOwns(std::string_view id) const {
    return disk_file::textFingerprint(
        provisioning::poolPath(directory_.parent_path() / "provisioning", id)).has_value();
}

std::unique_ptr<RaidController> RaidArrayManager::makeController(RaidConfiguration configuration,
                                                                 Members members,
                                                                 std::vector<bool> unavailable) {
    const bool pending = hasPendingWrite(configuration);
    if (configuration.level == RaidLevel::Raid0) {
        return std::make_unique<Raid0>(std::move(configuration), std::move(members), std::move(unavailable), pending);
    }
    const auto marker = directory_ / (configuration.id + ".pending");
    const auto contents = configuration.level == RaidLevel::Raid1 ? mirrorMarker : parityMarker;
    RaidWriteJournal journal{
        [marker, contents] { disk_file::publishText(marker, contents); },
        [marker, contents] { disk_file::removeText(marker, contents); }};
    if (configuration.level == RaidLevel::Raid5) {
        return std::make_unique<Raid5>(std::move(configuration), std::move(members), std::move(journal),
                                       pending, std::move(unavailable));
    }
    if (configuration.level == RaidLevel::Raid6) {
        return std::make_unique<Raid6>(std::move(configuration), std::move(members), std::move(journal),
                                       pending, &VirtualDisk::flush, std::move(unavailable));
    }
    return std::make_unique<Raid1>(std::move(configuration), std::move(members), std::move(journal),
                                   pending, std::move(unavailable));
}

RaidController& RaidArrayManager::createArray(std::string_view id, RaidLevel level,
                                               std::vector<std::string> ids, std::uint64_t stripe) {
    if (!isValidArrayId(id)) {
        throw std::invalid_argument("Invalid RAID array identifier. Use array0, array1, and so on.");
    }
    validateMembers(ids);
    validateRaidLayout(level, ids.size(), stripe);
    const auto catalog = loadCatalog();
    if (catalog.find(id) != catalog.end()) {
        throw std::invalid_argument("RAID array already exists: " + std::string(id));
    }
    if (poolOwns(id)) {
        throw std::invalid_argument("Cannot recreate a RAID array owned by a storage pool");
    }
    for (const auto& disk : ids) {
        if (spares_.find(disk)) {
            throw std::invalid_argument(disk + " is registered as a spare");
        }
        for (const auto& entry : catalog) {
            if (owns(entry.second, disk)) {
                throw std::invalid_argument(disk + " already belongs to RAID array " + entry.first);
            }
        }
    }

    auto members = openMembers(ids);
    const auto capacity = calculateRaidCapacity(level, ids.size(), members.front()->getBlockCount(), stripe);
    RaidConfiguration configuration{std::string(id), level, std::move(ids), stripe, capacity};
    configuration.layout = parityLayout(level);
    if (failures_.load(configuration).persisted) {
        throw std::invalid_argument("Existing failure state prevents creating RAID array " + configuration.id);
    }
    for (const auto& member : members) {
        configuration.memberIdentities.push_back(member->imageIdentity());
    }
    auto controller = makeController(configuration, members);
    if (controller->status() != RaidStatus::Healthy) {
        throw std::invalid_argument("Cannot create an array with an existing RAID write marker");
    }
    for (std::size_t index = 0; index < members.size(); ++index) {
        requireBlank(*members[index], configuration.members[index]);
    }

    const auto entry = arrays_.emplace(configuration.id, std::move(controller)).first;
    try {
        disk_file::publishText(directory_ / (configuration.id + ".raid"), serialize(configuration));
    } catch (...) {
        arrays_.erase(entry);
        throw;
    }
    return *entry->second;
}

RaidController& RaidArrayManager::openArray(std::string_view id) {
    auto configuration = findArray(id);
    const auto existing = arrays_.find(id);
    if (existing != arrays_.end()) {
        return *existing->second;
    }
    auto resolved = resolveMembers(configuration, true);
    std::vector<bool> unavailable;
    unavailable.reserve(resolved.status.size());
    for (const auto& member : resolved.status) {
        unavailable.push_back(!member.available);
    }
    auto controller = makeController(std::move(configuration), std::move(resolved.disks), std::move(unavailable));
    const auto key = controller->configuration().id;
    controller->poolOwned_ = poolOwns(key);
    return *arrays_.emplace(key, std::move(controller)).first->second;
}

bool RaidArrayManager::failMember(std::string_view arrayId, std::string_view memberId) {
    if (!VirtualDiskManager::isValidDiskId(memberId)) {
        throw std::invalid_argument("Invalid RAID member disk identifier");
    }
    const auto configuration = findArray(arrayId);
    const auto found = std::find(configuration.members.begin(), configuration.members.end(), memberId);
    if (found == configuration.members.end()) {
        throw std::invalid_argument("Disk is not a member of " + configuration.id);
    }
    auto state = failures_.load(configuration);
    const auto index = static_cast<std::size_t>(found - configuration.members.begin());
    const bool changed = failures_.failMember(configuration, state, index, FailureCause::Injected);
    const auto cached = arrays_.find(arrayId);
    if (cached != arrays_.end()) {
        cached->second->markUnavailable(memberId);
    }
    return changed;
}

ArrayHealthReport RaidArrayManager::getArrayStatus(std::string_view id) {
    auto report = inspectArray(findArray(id));
    report.poolOwned = poolOwns(id);
    return report;
}

ArrayHealthReport RaidArrayManager::inspectArray(const RaidConfiguration& configuration) {
    auto resolved = resolveMembers(configuration, false);
    const bool pending = hasPendingWrite(configuration);
    const auto cached = arrays_.find(configuration.id);
    std::size_t unavailable = 0;
    for (std::size_t index = 0; index < resolved.status.size(); ++index) {
        auto& member = resolved.status[index];
        if (cached != arrays_.end()) {
            member.available = member.available && cached->second->memberAvailable(index);
        }
        unavailable += !member.available;
    }
    const bool inconsistent = pending ||
        (cached != arrays_.end() && cached->second->status() == RaidHealth::Inconsistent);
    return {configuration.level, calculateRaidHealth(configuration.level, unavailable, inconsistent),
            failureTolerance(configuration.level), pending, std::move(resolved.status)};
}

std::vector<RaidConfiguration> RaidArrayManager::listArrays() {
    auto catalog = loadCatalog();
    std::vector<RaidConfiguration> arrays;
    arrays.reserve(catalog.size());
    for (auto& entry : catalog) {
        arrays.push_back(std::move(entry.second));
    }
    return arrays;
}

std::vector<ArrayListing> RaidArrayManager::listArraysWithHealth() {
    auto catalog = loadCatalog();
    std::vector<ArrayListing> arrays;
    arrays.reserve(catalog.size());
    for (auto& entry : catalog) {
        const auto health = inspectArray(entry.second).health;
        arrays.push_back({std::move(entry.second), health});
    }
    return arrays;
}

void RaidArrayManager::requireUnownedDisk(std::string_view id) {
    const auto catalog = loadCatalog();
    for (const auto& entry : catalog) {
        if (owns(entry.second, id)) {
            throw std::invalid_argument("Raw write rejected. " + std::string(id) +
                                        " belongs to RAID array " + entry.first);
        }
    }
    if (spares_.find(id)) {
        throw std::invalid_argument("Raw write rejected. " + std::string(id) + " is registered as a spare");
    }
}

void RaidArrayManager::registerSpare(std::string_view id) {
    if (!VirtualDiskManager::isValidDiskId(id)) {
        throw std::invalid_argument("Invalid spare disk identifier");
    }
    const auto catalog = loadCatalog();
    for (const auto& entry : catalog) {
        if (owns(entry.second, id)) {
            throw std::invalid_argument("Cannot register an array-owned member as a spare");
        }
    }
    if (spares_.find(id)) {
        throw std::invalid_argument("Disk is already registered as a spare: " + std::string(id));
    }
    spares_.registerBlank(id, disks_.openDisk(id));
}

void RaidArrayManager::closeArrays() noexcept {
    arrays_.clear();
}

}  // namespace raid
