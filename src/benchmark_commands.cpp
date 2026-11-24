#include "benchmark_commands.h"

#include "benchmark/benchmark.h"
#include "benchmark/rebuild_study.h"
#include "core/config.h"

#include <charconv>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using namespace raid::benchmark;

std::uint64_t order(std::string_view value) {
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result == 0 || result > 10000)
        throw std::invalid_argument("Invalid matrix execution order");
    return result;
}
}

int runBenchmarkCommand(const std::vector<std::string>& args) {
    if (args.size() < 2) throw std::invalid_argument("Expected a benchmark subcommand");
    const auto& action = args[1];
    const auto root = raid::config::projectRoot() / "benchmarks";
    if (action == "rebuild") {
        if (args.size() != 4 || args[2] != "--config") throw std::invalid_argument("Expected benchmark rebuild --config <file>");
        const auto result = runStudy(readStudy(fs::u8path(args[3])), root);
        std::cout << Json{{"directory", result.directory.u8string()}, {"status", result.passed ? "COMPLETED" : "FAILED"}}.dump() << '\n';
        return result.passed ? 0 : 1;
    }
    if (action == "list-presets" && args.size() == 2) {
        std::cout << Json({
            "raid5_random_write.json", "thin_first_write.json", "thin_overwrite.json",
            "rebuild_read_healthy.json", "rebuild_read_concurrent.json"
        }).dump(2) << '\n';
        return 0;
    }
    std::map<std::string, std::string> options;
    const auto offset = action == "info" ? 3U : 2U;
    if (args.size() < offset || (args.size() - offset) % 2)
        throw std::invalid_argument("Expected benchmark options in flag/value pairs");
    for (std::size_t i = offset; i < args.size(); i += 2) {
        const auto& flag = args[i];
        if (flag != "--output-dir" && (action == "info" || (flag != "--config" &&
            (action != "run" || (flag != "--matrix-id" && flag != "--matrix-order" && flag != "--python-version")))))
            throw std::invalid_argument("Unsupported benchmark option: " + flag);
        if (!options.emplace(flag, args[i + 1]).second || args[i + 1].empty())
            throw std::invalid_argument("Duplicate or empty benchmark option");
    }
    const auto group = options["--output-dir"];
    if (action == "info") {
        std::cout << info(root, group, args[2]).dump(2) << '\n';
        return 0;
    }
    if (action != "run" && action != "validate" && action != "dry-run")
        throw std::invalid_argument("Expected benchmark run, validate, dry-run, list-presets or info");
    const auto output = outputRoot(root, group);
    if (!options.count("--config")) throw std::invalid_argument("Benchmark requires --config <file>");
    const auto config = loadConfig(fs::u8path(options.at("--config")));
    if (action == "validate" || action == "dry-run") {
        const auto used = Json{{"configuration", describe(config)}, {"source", "MEASURED"}, {"queue_depth", 1},
            {"disk_capacity_mib", config.diskMiB}, {"raw_capacity_bytes", config.disks * config.diskMiB * mib},
            {"usable_capacity_bytes", usableBlocks(config) * raid::raidBlockSize},
            {"temporary_image_bytes", imageBudgetMiB(config) * mib},
            {"target_logical_bytes", config.operations * config.repetitions * config.blocks * raid::raidBlockSize},
            {"raw_csv_budget_bytes", config.maxRawBytes}, {"max_concurrent_runs", 1},
            {"expected_result_parent", output.u8string()}};
        std::cout << used.dump(2) << '\n';
        return 0;
    }

    Json invocation = Json::object();
    if (options.count("--matrix-id")) {
        if (!safeName(options.at("--matrix-id"))) throw std::invalid_argument("Invalid matrix identifier");
        invocation["matrix_id"] = options.at("--matrix-id");
    }
    if (options.count("--matrix-order")) invocation["matrix_order"] = order(options.at("--matrix-order"));
    if (options.count("--python-version")) {
        const auto& version = options.at("--python-version");
        if (version.size() > 24 || version.find_first_not_of("0123456789.") != std::string::npos)
            throw std::invalid_argument("Expected a numeric Python version");
        invocation["python"] = version;
    }
    const auto result = run(config, root, group, invocation);
    std::cout << Json{{"run_id", result.directory.filename().u8string()}, {"directory", result.directory.u8string()},
                       {"status", result.passed ? "COMPLETED" : "FAILED"}}.dump() << '\n';
    return result.passed ? 0 : 1;
}
