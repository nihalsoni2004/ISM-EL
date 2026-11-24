#include "core/engine.h"
#include "benchmark_commands.h"
#include "disk_commands.h"
#include "file_commands.h"
#include "ipc/json_server.h"
#include "provisioning_commands.h"
#include "raid_commands.h"
#include "rebuild_commands.h"

#define NOMINMAX
#include <windows.h>

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void printHelp() {
    std::cout << "Usage: raid_simulator.exe <command>\n"
              << "  --help       Show available commands\n"
              << "  --version    Show the application version\n"
              << "  --status     Initialize the engine and report its status\n"
              << "  --self-test  Check the engine lifecycle\n"
              << "  --serve-json --storage-root <absolute-path>  Serve versioned JSON requests on stdin/stdout\n"
              << "  benchmark <run|validate|dry-run> --config <file> [--output-dir <group>]\n"
              << "  benchmark list-presets | benchmark info <run-id> [--output-dir <group>]\n"
              << "  benchmark rebuild --config <file>\n"
              << "  disk create <id> [capacity-MiB]  Create a new disk (default 100 MiB)\n"
              << "  disk list                       List virtual disks\n"
              << "  disk info <id>                  Show disk information\n"
              << "  disk write <id> <lba> <text>    Write and zero-pad one block\n"
              << "  disk read <id> <lba>            Show a block preview\n"
              << "  disk verify <id>                Check image size and read boundary blocks\n"
              << "  raid create <id> <raid0|raid1|raid5|raid6> <disk> <disk> [...] [--stripe <blocks>]\n"
              << "  raid list                       List configured arrays\n"
              << "  raid info <id>                  Show RAID geometry and availability\n"
              << "  raid status <id>                Show array health and permitted I/O\n"
              << "  raid members <id>               Show member states and image presence\n"
              << "  raid fail <id> <disk>           Persistently mark a member FAILED\n"
              << "  raid write <id> <lba> <text>    Write and zero-pad one RAID block\n"
              << "  raid write-blocks <id> <lba> <count> <text>    Repeat a padded payload across blocks\n"
              << "  raid read <id> <lba>            Show a RAID block preview\n"
              << "  raid map <id> <lba>             Show physical member locations\n"
              << "  raid verify <id>                Check samples, mirrors, or distributed parity\n"
              << "  raid demo-degraded-read <id> <disk>[,<disk>] <lba>\n"
              << "  spare register <disk> | spare list\n"
              << "  raid rebuild prepare <array> <failed-member> <spare>\n"
              << "  raid rebuild <run|resume> <job> [--batches <count>]\n"
              << "  raid rebuild <status|pause> <job> | raid rebuild list\n"
              << "  pool create <pool> <array> [--chunk-blocks <count>] [--max-ratio <integer>]\n"
              << "  pool list | pool <info|verify> <pool>\n"
              << "  pool watermarks <pool> [<warning-percent> <critical-percent>]\n"
              << "  lun create <lun> <pool> <capacity-MiB> [--mode file] | lun list | lun info <lun>\n"
              << "  lun <read|map> <lun> <lba> | lun write <lun> <lba> <text>\n"
              << "  lun read-blocks <lun> <lba> <count>\n"
              << "  lun write-blocks <lun> <lba> <count> <text>\n"
              << "  file import <lun> <source-path> | file list [<lun>] | file info <file-id>\n"
              << "  file export <file-id> <destination-path> | file verify <file-id> | file verify-all <lun>\n";
}

int runSelfTest() {
    raid::StorageEngine engine;
    using raid::EngineState;

    if (engine.getStatus().state != EngineState::Created ||
        engine.shutdown() ||
        !engine.initialize() ||
        engine.initialize() ||
        engine.getStatus().state != EngineState::Initialized ||
        !engine.shutdown() ||
        engine.shutdown() ||
        engine.initialize() ||
        engine.getStatus().state != EngineState::Shutdown) {
        std::cerr << "Engine lifecycle self-test failed.\n";
        return 1;
    }

    std::cout << "Engine lifecycle self-test passed.\n";
    return 0;
}

int printEngineStatus() {
    raid::StorageEngine engine;
    if (!engine.initialize() || engine.getStatus().state != raid::EngineState::Initialized) {
        std::cerr << "Engine did not enter the initialized state.\n";
        return 1;
    }
    std::cout << "Storage engine status: INITIALIZED\n";
    if (!engine.shutdown()) {
        std::cerr << "Engine shutdown failed.\n";
        return 1;
    }
    return 0;
}

std::string utf8(const wchar_t* argument) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument, -1, nullptr, 0, nullptr, nullptr);
    if (size == 0) {
        throw std::invalid_argument("Command argument is not valid Unicode");
    }
    std::string text(size, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument, -1, text.data(), size, nullptr, nullptr) == 0) {
        throw std::invalid_argument("Could not encode command argument as UTF-8");
    }
    text.pop_back();
    return text;
}

int run(const std::vector<std::string>& args) {
    if (args.empty()) {
        printHelp();
        return 0;
    }
    const std::string_view command = args.front();
    if (command == "--serve-json" && args.size() == 3 && args[1] == "--storage-root") {
        return raid::ipc::serve(std::filesystem::u8path(args[2]));
    }
    if (command == "benchmark") {
        return runBenchmarkCommand(args);
    }
    if (command == "disk") {
        return runDiskCommand(args);
    }
    if (command == "raid") {
        return runRaidCommand(args);
    }
    if (command == "spare") {
        return runSpareCommand(args);
    }
    if (command == "pool") {
        return runPoolCommand(args);
    }
    if (command == "lun") {
        return runLunCommand(args);
    }
    if (command == "file") {
        return runFileCommand(args);
    }
    if (args.size() != 1) {
        throw std::invalid_argument("Expected one command. Run with --help for usage.");
    }
    if (command == "--help") {
        printHelp();
        return 0;
    }
    if (command == "--version") {
        std::cout << raid::StorageEngine{}.getVersion() << '\n';
        return 0;
    }
    if (command == "--status") {
        return printEngineStatus();
    }
    if (command == "--self-test") {
        return runSelfTest();
    }
    throw std::invalid_argument("Unknown command: " + std::string(command) + ". Run with --help for usage.");
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    const auto previousCodePage = GetConsoleOutputCP();
    const bool changedCodePage = previousCodePage != 0 && previousCodePage != CP_UTF8 &&
                                 SetConsoleOutputCP(CP_UTF8);
    int result = 0;
    try {
        std::vector<std::string> args;
        args.reserve(argc - 1);
        for (int index = 1; index < argc; ++index) {
            args.push_back(utf8(argv[index]));
        }
        result = run(args);
    } catch (const std::invalid_argument& error) {
        std::cerr << "Error: " << error.what() << '\n';
        result = 2;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        result = 1;
    }
    std::cout.flush();
    if (changedCodePage) {
        SetConsoleOutputCP(previousCodePage);
    }
    return result;
}
