#include "benchmark/benchmark.h"
#include "test_support.h"

#include <algorithm>
#include <fstream>
#include <iostream>

namespace {
using namespace raid::benchmark;
using test::require;

Json input(std::string_view workload, int level) {
    return {{"schema_version", 1}, {"benchmark_name", "bench, \"fixture\""}, {"raid_level", level},
            {"disk_capacity_mib", 1}, {"workload", workload}, {"operation_count", 4}, {"warmup_operations", 1},
            {"repetitions", 1}, {"seed", 421}, {"footprint_blocks", 128}, {"validation", "FULL"}};
}

template <typename Callback> void rejects(Callback callback, const char* message) {
    test::rejects<std::invalid_argument>(callback, message);
}

void configuration() {
    for (const auto level : {0, 1, 5, 6}) {
        for (const auto mode : {"RANDOM_READ", "RANDOM_WRITE", "SEQUENTIAL_READ", "SEQUENTIAL_WRITE",
                                "MIXED_RW", "THIN_FIRST_WRITE", "THIN_OVERWRITE", "READ_ONLY_BASELINE"}) {
            const auto config = parseConfig(input(mode, level));
            require(parseConfig(describe(config)).seed == config.seed, "config normalization was not round-trippable");
        }
        auto raw = input("RANDOM_READ", level);
        raw.erase("disk_capacity_mib");
        raw["capacity_mode"] = "EQUAL_RAW";
        raw["target_capacity_mib"] = 16;
        const auto equalRaw = parseConfig(raw);
        require(equalRaw.diskMiB * equalRaw.disks == 16, "equal raw geometry differs");
        raw["capacity_mode"] = "EQUAL_USABLE";
        raw["target_capacity_mib"] = 12;
        require(usableBlocks(parseConfig(raw)) * raid::raidBlockSize == 12 * mib, "equal usable geometry differs");
    }
    const Json invalid{{"disk_capacity_mib", 0}, {"raid_level", 3}, {"stripe_unit_blocks", 3},
           {"request_size_bytes", 8192}, {"operation_count", 0}, {"queue_depth", 2}, {"seed", -1},
           {"rebuild_contention", true}, {"limits", {{"max_disks", 2}}}, {"source", "MODELED"}, {"bad", 1}};
    for (const auto& item : invalid.items()) {
        auto value = input("RANDOM_READ", 5);
        value[item.key()] = item.value();
        rejects([&] { parseConfig(value); }, "invalid configuration accepted");
    }
    auto excessive = input("THIN_FIRST_WRITE", 5);
    excessive["operation_count"] = 50;
    rejects([&] { parseConfig(excessive); }, "out-of-space thin scenario accepted");
    auto csv = input("RANDOM_READ", 5);
    csv["operation_count"] = 20;
    csv["limits"] = {{"max_raw_csv_bytes", 8192}};
    rejects([&] { parseConfig(csv); }, "raw CSV budget was ignored");
    test::TempDirectory root;
    for (const auto field : {"..", "../outside", "G:/elsewhere", "\\absolute", "x:y", "x/y"})
        rejects([&] { outputRoot(root.path(), field); }, "unsafe output group accepted");
    require(outputRoot(root.path(), "group") == root.path() / "results/group", "valid nested group rejected");
    require(fs::is_empty(root.path()), "validation modified storage");
}

bool same(const Operation& left, const Operation& right) {
    return left.write == right.write && left.lba == right.lba && left.blocks == right.blocks && left.pattern == right.pattern;
}

void workload() {
    auto json = input("MIXED_RW", 5);
    json["operation_count"] = 11;
    for (const auto ratio : {30, 50, 70}) {
        json["mixed_read_percent"] = ratio;
        const auto config = parseConfig(json);
        const auto first = makeWorkload(config, 1);
        const auto second = makeWorkload(config, 1);
        const auto next = makeWorkload(config, 2);
        require(std::equal(first.begin(), first.end(), second.begin(), same), "same seed and repetition changed");
        require(!std::equal(first.begin(), first.end(), next.begin(), same), "repetition seed was not varied");
        const auto reads = std::count_if(first.begin() + 1, first.end(), [](const auto& op) { return !op.write; });
        require(reads == (11 * ratio + 50) / 100, "mixed read ratio rounding differs");
        for (const auto& op : first) require(op.lba + op.blocks <= config.footprint, "out-of-range random request");
    }
    const auto seq = makeWorkload(parseConfig(input("SEQUENTIAL_WRITE", 6)), 1);
    for (std::size_t i = 0; i < seq.size(); ++i) {
        require(seq[i].lba == (i % 2) * 64 && seq[i].blocks == 64, "wrong sequential wrap");
    }
    auto first = input("THIN_FIRST_WRITE", 5);
    first["operation_count"] = 2;
    first["thin_offset_blocks"] = 15;
    first["request_size_bytes"] = 8192;
    auto overwrite = first;
    overwrite["workload"] = "THIN_OVERWRITE";
    const auto firstOps = makeWorkload(parseConfig(first), 1);
    const auto overwriteOps = makeWorkload(parseConfig(overwrite), 1);
    require(std::equal(firstOps.begin(), firstOps.end(), overwriteOps.begin(), same), "thin comparison workload is unmatched");
    std::array<std::uint8_t, raid::raidBlockSize> a{}, b{};
    fillBlock(a.data(), 7, 3, 2);
    fillBlock(b.data(), 7, 3, 2);
    require(a == b, "data pattern is not reproducible");
    fillBlock(b.data(), 7, 3, 3);
    require(a != b, "different overwrites used identical data");
}

void statistics() {
    auto c = parseConfig(input("RANDOM_READ", 5));
    c.validation = Validation::None;
    c.operations = 12000;
    Repetition repetition;
    repetition.elapsed = 1;
    require(summarizeRun(c, repetition)["latency_p99_ms"].is_null(), "empty latency should be null");
    for (std::uint64_t i = 1; i <= c.operations; ++i) {
        Sample sample{};
        sample.operation = {false, 0, 1, 0};
        sample.durationNs = i * 1000;
        sample.success = true;
        repetition.samples.push_back(sample);
    }
    const auto counts = summarizeRun(c, repetition);
    require(counts["iops"] == 12000.0 && counts["latency_count"] == 12000 &&
        counts["latency_p50_ms"] == 6.0 && counts["latency_p95_ms"] == 11.4 && counts["latency_p99_ms"] == 11.88 &&
        counts["latency_mean_ms"] == 6.0005 && counts["validation_status"] == "NOT_CHECKED",
        "full-run latency was miscomputed or limited by the telemetry ring");
    repetition.samples.back().success = false;
    require(summarizeRun(c, repetition)["failed_operations"] == 1 && !valid(c, repetition), "failed request was accepted");
}

Repetition trial(const Config& c) {
    const test::TempDirectory isolated;
    return measure(c, 1, isolated.path(), raid::telemetry::Clock::now() + std::chrono::seconds(30));
}

void raidTrials() {
    for (const auto level : {0, 1, 5, 6}) {
        for (const auto* shape : {"RANDOM_READ", "RANDOM_WRITE", "SEQUENTIAL_READ", "SEQUENTIAL_WRITE"}) {
            const auto c = parseConfig(input(shape, level));
            const auto run = trial(c);
            if (!valid(c, run)) throw std::runtime_error(run.error);
            require(run.persisted && run.samples[0].validation == true, "exact readback did not pass");
            if (!run.samples[0].operation.write) continue;
            const auto first = run.samples[0].physical;
            if (c.workload == Workload::RandomWrite) {
                const auto parity = level == 5 ? 2U : level == 6 ? 3U : 0U;
                require(first.read.attempted == parity && first.write.attempted == (parity ? parity : level == 1 ? 2 : 1),
                        "unexpected small write amplification");
            }
            if (c.workload == Workload::SequentialWrite && level == 6) {
                require(first.read.attempted == 0 && first.write.attempted == 8 && first.write.blocks == 128,
                        "full-stripe writes were not measured");
            }
        }
    }
}

void thinTrials() {
    for (const auto first : {true, false}) {
        auto json = input(first ? "THIN_FIRST_WRITE" : "THIN_OVERWRITE", 5);
        json["operation_count"] = 2;
        json["thin_offset_blocks"] = 15;
        json["request_size_bytes"] = 8192;
        const auto c = parseConfig(json);
        const auto run = trial(c);
        if (!valid(c, run)) throw std::runtime_error(run.error);
        const auto measured = summarizeRun(c, run);
        require(measured["new_chunks"] == (first ? 4 : 0), "thin cross-chunk behavior was not measured");
        require(*run.allocatedAfter - *run.allocatedBefore == (first ? 4 * 65536 : 0), "unexpected capacity growth");
    }
}

void results() {
    const test::TempDirectory root;
    const auto config = parseConfig(input("RANDOM_WRITE", 1));
    const auto first = run(config, root.path(), "output");
    const auto second = run(config, root.path(), "output");
    require(first.passed && second.passed && first.directory != second.directory, "run path was reused");
    const auto manifest = info(root.path(), "output", first.directory.filename().u8string());
    require(manifest["schema_version"] == 1 && manifest["measurement_source"] == "MEASURED" &&
        manifest["configuration"]["seed"] == 421 && manifest["repetitions"][0]["validation_status"] == "PASSED" &&
        manifest["host"].contains("cpu_identifier") && manifest["host"].contains("cpu_model") && manifest["compiler"].is_string(),
        "missing manifest metadata");
    const auto header = [](const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        std::string line;
        std::getline(input, line);
        return line;
    };
    require(header(first.directory / "benchmark_runs.csv").find("duration_ns") != std::string::npos &&
        header(first.directory / "benchmark_results.csv").find("latency_p99_ms") != std::string::npos &&
        escapeCsv("bench, \"fixture\"") == "\"bench, \"\"fixture\"\"\"", "incorrect output schema or CSV escaping");
}

void rebuildBaseline() {
    auto json = input("READ_ONLY_BASELINE", 1);
    json["disk_capacity_mib"] = 4;
    json["rebuild_contention"] = true;
    json["operation_count"] = 8;
    const auto c = parseConfig(json);
    const auto run = trial(c);
    if (!valid(c, run)) throw std::runtime_error(run.error);
    require(run.rebuildSeconds && run.rebuilt == 1024 && run.rebuildState == "COMPLETED" &&
        std::any_of(run.samples.begin(), run.samples.end(), [](const auto& event) { return event.rebuildOverlap; }),
        "missing overlap or durable rebuild measurement");
    require(std::none_of(run.samples.begin(), run.samples.end(), [](const auto& event) {
        return event.operation.write || event.physical.write.attempted != 0;
    }), "background writes were attributed to foreground reads");
}
}

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc == 2 ? argv[1] : "";
        if (mode == "config") configuration();
        else if (mode == "workload") workload();
        else if (mode == "statistics") statistics();
        else if (mode == "raid") raidTrials();
        else if (mode == "thin") thinTrials();
        else if (mode == "results") results();
        else if (mode == "rebuild") rebuildBaseline();
        else throw std::invalid_argument("Expected benchmark test mode");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
