#include "benchmark/benchmark.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>

namespace raid::benchmark {
namespace {

constexpr std::array<std::string_view, 8> workloads{
    "RANDOM_READ", "RANDOM_WRITE", "SEQUENTIAL_READ", "SEQUENTIAL_WRITE", "MIXED_RW",
    "THIN_FIRST_WRITE", "THIN_OVERWRITE", "READ_ONLY_BASELINE"
};
constexpr std::array<std::string_view, 3> validation{"FULL", "SAMPLED", "NONE"};
constexpr std::array<std::string_view, 3> capacities{"PER_MEMBER", "EQUAL_RAW", "EQUAL_USABLE"};

template <typename Enum, std::size_t N>
Enum select(std::string_view text, const std::array<std::string_view, N>& values) {
    const auto choice = std::find(values.begin(), values.end(), text);
    if (choice == values.end()) throw std::invalid_argument("Unsupported benchmark option: " + std::string(text));
    return static_cast<Enum>(choice - values.begin());
}

std::uint64_t integer(const Json& value, const char* key, std::uint64_t fallback,
                      std::uint64_t minimum, std::uint64_t maximum) {
    const auto found = value.find(key);
    if (found == value.end()) return fallback;
    if (!found->is_number_integer() || (!found->is_number_unsigned() && found->get<std::int64_t>() < 0)) {
        throw std::invalid_argument(std::string(key) + " must be a whole nonnegative number");
    }
    const auto result = found->get<std::uint64_t>();
    if (result < minimum || result > maximum) throw std::invalid_argument(std::string(key) + " is outside its supported range");
    return result;
}

std::uint64_t mix(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

std::uint64_t below(std::mt19937_64& rng, std::uint64_t bound) {
    const auto threshold = (std::uint64_t{0} - bound) % bound;
    std::uint64_t value;
    do { value = rng(); } while (value < threshold);
    return value % bound;
}

template <typename T>
void shuffle(std::vector<T>& values, std::mt19937_64& rng) {
    for (auto remaining = values.size(); remaining > 1; --remaining) {
        const auto choice = static_cast<std::size_t>(below(rng, remaining));
        std::swap(values[remaining - 1], values[choice]);
    }
}

bool sequential(Workload workload) noexcept {
    return workload == Workload::SequentialRead || workload == Workload::SequentialWrite;
}

std::uint64_t thinSpan(const Config& config) noexcept {
    return (config.thinOffset + config.blocks + chunkBlocks - 1) / chunkBlocks;
}

}  // namespace

std::string_view name(Workload value) noexcept { return workloads[static_cast<std::size_t>(value)]; }
std::string_view name(Validation value) noexcept { return validation[static_cast<std::size_t>(value)]; }
std::string_view name(CapacityMode value) noexcept { return capacities[static_cast<std::size_t>(value)]; }
bool thin(Workload value) noexcept { return value == Workload::ThinFirst || value == Workload::ThinOverwrite; }

std::uint64_t usableBlocks(const Config& config) {
    return calculateRaidCapacity(config.level, static_cast<std::size_t>(config.disks),
                                 config.diskMiB * mib / raidBlockSize, config.stripe);
}
std::uint64_t imageBudgetMiB(const Config& config) noexcept {
    return (config.disks + (config.rebuild ? 1 : 0)) * config.diskMiB;
}

Config parseConfig(const Json& input) {
    using std::string_view;
    constexpr std::array<string_view, 24> allowed{
        "schema_version", "benchmark_name", "raid_level", "disk_count", "disk_capacity_mib", "stripe_unit_blocks",
        "workload", "request_size_bytes", "operation_count", "warmup_operations", "repetitions", "seed", "queue_depth",
        "validation", "source", "footprint_blocks", "mixed_read_percent", "thin_offset_blocks", "validation_stride",
        "capacity_mode", "target_capacity_mib", "rebuild_contention", "limits", "cache_policy"
    };
    if (!input.is_object()) throw std::invalid_argument("Benchmark configuration must be a JSON object");
    for (const auto& item : input.items()) {
        if (std::find(allowed.begin(), allowed.end(), item.key()) == allowed.end())
            throw std::invalid_argument("Unknown benchmark field: " + item.key());
    }
    if (!input.contains("schema_version") || integer(input, "schema_version", 0, 1, 1) != 1)
        throw std::invalid_argument("Expected benchmark schema_version 1");
    if (input.value("source", std::string{"MEASURED"}) != "MEASURED" ||
        input.value("cache_policy", std::string{"HOST_CACHE_ENABLED_PREPARED"}) != "HOST_CACHE_ENABLED_PREPARED")
        throw std::invalid_argument("This runner supports measured I/O with the host cache enabled");
    const auto level = integer(input, "raid_level", 99, 0, 6);
    if (level != 0 && level != 1 && level != 5 && level != 6) throw std::invalid_argument("Expected RAID 0, 1, 5 or 6");

    Config config{};
    config.name = input.value("benchmark_name", std::string{});
    if (config.name.empty() || config.name.size() > 64 ||
        std::any_of(config.name.begin(), config.name.end(), [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; }))
        throw std::invalid_argument("Benchmark name must contain 1 to 64 bytes and no control characters");
    config.level = static_cast<RaidLevel>(level);
    config.workload = select<Workload>(input.value("workload", std::string{}), workloads);
    config.validation = select<Validation>(input.value("validation", std::string{"SAMPLED"}), validation);
    config.capacityMode = select<CapacityMode>(input.value("capacity_mode", std::string{"PER_MEMBER"}), capacities);
    config.disks = integer(input, "disk_count", level == 1 ? 2 : 4, 2, 8);
    config.stripe = integer(input, "stripe_unit_blocks", level == 1 ? 1 : 16, 1, 256);
    validateRaidLayout(config.level, static_cast<std::size_t>(config.disks), config.stripe);
    config.targetMiB = integer(input, "target_capacity_mib", 0, 1, 1024);
    if (config.capacityMode == CapacityMode::PerMember) {
        if (config.targetMiB) throw std::invalid_argument("target_capacity_mib requires a capacity matching mode");
        config.diskMiB = integer(input, "disk_capacity_mib", 8, 1, 256);
    } else {
        const auto units = config.capacityMode == CapacityMode::EqualRaw ? config.disks :
            dataUnitCount(config.level, static_cast<std::size_t>(config.disks));
        if (!config.targetMiB || config.targetMiB % units || input.contains("disk_capacity_mib"))
            throw std::invalid_argument("Target capacity must divide evenly among the selected raw or data units");
        config.diskMiB = config.targetMiB / units;
    }
    if (config.diskMiB > 256) throw std::invalid_argument("Individual disk exceeds the 256 MiB hard limit");
    const auto capacity = usableBlocks(config);
    const auto bytes = integer(input, "request_size_bytes", sequential(config.workload) ? 262144 : 4096, 4096, 262144);
    if (bytes % raidBlockSize || (sequential(config.workload) && bytes != 262144) ||
        (!sequential(config.workload) && !thin(config.workload) && bytes != 4096))
        throw std::invalid_argument("Request size must match the workload and be a multiple of 4096");
    config.blocks = bytes / raidBlockSize;
    config.footprint = integer(input, "footprint_blocks", std::min<std::uint64_t>(capacity, 1536), config.blocks, capacity);
    config.operations = integer(input, "operation_count", 1000, 1, 100000);
    config.warmup = integer(input, "warmup_operations", 100, 0, 10000);
    config.repetitions = integer(input, "repetitions", 5, 1, 20);
    config.seed = integer(input, "seed", 12345, 0, (std::numeric_limits<std::uint64_t>::max)());
    (void)integer(input, "queue_depth", 1, 1, 1);
    config.readPercent = integer(input, "mixed_read_percent", 70, 0, 100);
    config.thinOffset = integer(input, "thin_offset_blocks", 0, 0, chunkBlocks - 1);
    config.sampleStride = integer(input, "validation_stride", 16, 1, 10000);
    config.rebuild = input.value("rebuild_contention", false);
    if (config.rebuild && (config.level == RaidLevel::Raid0 || config.workload != Workload::ReadBaseline))
        throw std::invalid_argument("Rebuild contention requires a read-only workload and a redundant array");
    if (config.thinOffset && !thin(config.workload)) throw std::invalid_argument("thin_offset_blocks requires a thin workload");
    if (thin(config.workload) && (config.footprint % chunkBlocks ||
        config.operations + config.warmup > config.footprint / (chunkBlocks * thinSpan(config))))
        throw std::invalid_argument("The thin workload needs one distinct aligned chunk group per warm-up and measured request");

    const auto limits = input.value("limits", Json::object());
    if (!limits.is_object()) throw std::invalid_argument("limits must be a JSON object");
    for (const auto& item : limits.items()) {
        if (item.key() != "max_image_mib" && item.key() != "max_disks" && item.key() != "max_operations" &&
            item.key() != "max_raw_csv_bytes" && item.key() != "timeout_seconds")
            throw std::invalid_argument("Unknown resource limit: " + item.key());
    }
    config.maxDisks = integer(limits, "max_disks", 8, 2, 9);
    config.maxImageMiB = integer(limits, "max_image_mib", 128, 1, 1024);
    config.maxOperations = integer(limits, "max_operations", 10000, 1, 500000);
    config.timeoutSeconds = integer(limits, "timeout_seconds", 900, 1, 3600);
    config.maxRawBytes = integer(limits, "max_raw_csv_bytes", 64 * mib, 8192, 512 * mib);
    if (config.disks + config.rebuild > config.maxDisks || imageBudgetMiB(config) > config.maxImageMiB ||
        (config.operations + config.warmup) * config.repetitions > config.maxOperations ||
        1024 + config.operations * config.repetitions * 512 > config.maxRawBytes)
        throw std::invalid_argument("Benchmark geometry or workload exceeds the configured resource budget");
    return config;
}

Config loadConfig(const fs::path& path) {
    if (!fs::is_regular_file(path) || fs::file_size(path) > 65536)
        throw std::invalid_argument("Benchmark configuration must be a regular JSON file of at most 64 KiB");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::invalid_argument("Could not read benchmark configuration");
    const std::string data{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    if (data.size() > 65536) throw std::invalid_argument("Benchmark configuration exceeds 64 KiB");
    try {
        return parseConfig(Json::parse(data));
    } catch (const Json::exception& error) {
        throw std::invalid_argument(std::string("Invalid benchmark JSON: ") + error.what());
    }
}

Json describe(const Config& c) {
    auto config = Json{{"schema_version", 1}, {"benchmark_name", c.name}, {"raid_level", static_cast<int>(c.level)},
        {"disk_count", c.disks}, {"stripe_unit_blocks", c.stripe}, {"workload", name(c.workload)},
        {"request_size_bytes", c.blocks * raidBlockSize}, {"operation_count", c.operations}, {"warmup_operations", c.warmup},
        {"repetitions", c.repetitions}, {"seed", c.seed}, {"queue_depth", 1}, {"validation", name(c.validation)},
        {"source", "MEASURED"}, {"cache_policy", "HOST_CACHE_ENABLED_PREPARED"}, {"footprint_blocks", c.footprint},
        {"mixed_read_percent", c.readPercent}, {"thin_offset_blocks", c.thinOffset}, {"validation_stride", c.sampleStride},
        {"capacity_mode", name(c.capacityMode)}, {"rebuild_contention", c.rebuild}, {"limits", {
            {"max_image_mib", c.maxImageMiB}, {"max_disks", c.maxDisks}, {"max_operations", c.maxOperations},
            {"max_raw_csv_bytes", c.maxRawBytes}, {"timeout_seconds", c.timeoutSeconds}}}};
    if (c.capacityMode == CapacityMode::PerMember) config["disk_capacity_mib"] = c.diskMiB;
    else config["target_capacity_mib"] = c.targetMiB;
    return config;
}

std::vector<Operation> makeWorkload(const Config& c, std::uint64_t repetition) {
    const auto seed = mix(c.seed ^ mix(repetition));
    std::mt19937_64 addresses(seed), ordering(mix(seed));
    const auto count = c.operations + c.warmup;
    std::vector<std::uint64_t> groups;
    if (thin(c.workload)) {
        groups.resize(static_cast<std::size_t>(c.footprint / (chunkBlocks * thinSpan(c))));
        for (std::size_t i = 0; i < groups.size(); ++i) groups[i] = i;
        shuffle(groups, addresses);
    }
    std::vector<Operation> result;
    result.reserve(static_cast<std::size_t>(count));
    std::uint64_t next = 0;
    for (const auto phase : {c.warmup, c.operations}) {
        const auto readCount = c.workload == Workload::Mixed ? (phase * c.readPercent + 50) / 100 : 0;
        std::vector<std::uint8_t> kinds(static_cast<std::size_t>(phase), 0);
        std::fill_n(kinds.begin(), static_cast<std::size_t>(readCount), std::uint8_t{1});
        if (c.workload == Workload::Mixed) shuffle(kinds, ordering);
        for (const auto isRead : kinds) {
            const auto index = result.size();
            std::uint64_t lba;
            if (thin(c.workload)) lba = groups[index] * thinSpan(c) * chunkBlocks + c.thinOffset;
            else if (sequential(c.workload)) {
                if (next > c.footprint - c.blocks) next = 0;
                lba = next;
                next += c.blocks;
            } else {
                lba = below(addresses, c.footprint - c.blocks + 1);
            }
            const bool write = c.workload == Workload::RandomWrite || c.workload == Workload::SequentialWrite ||
                thin(c.workload) || (c.workload == Workload::Mixed && !isRead);
            result.push_back({write, lba, c.blocks, write ? static_cast<std::uint64_t>(index) + 1 : 0});
        }
    }
    return result;
}

void fillBlock(std::uint8_t* out, std::uint64_t seed, std::uint64_t lba, std::uint64_t pattern) noexcept {
    const auto word = mix(seed ^ mix(lba) ^ mix(pattern + 0x435455ULL));
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(word >> (8 * i));
    for (std::size_t i = 0; i < raidBlockSize; i += bytes.size())
        std::copy(bytes.begin(), bytes.end(), out + i);
}

}  // namespace raid::benchmark
