#include "file_commands.h"

#include "block_commands.h"
#include "files/file_manager.h"

#include <iostream>
#include <stdexcept>

namespace {

std::uint64_t parseId(std::string_view id) {
    if (id.substr(0, 4) != "file") {
        throw std::invalid_argument("Use a file identifier such as file1");
    }
    const auto number = cli::parseNumber(id.substr(4), "File ID");
    if (number == 0) {
        throw std::invalid_argument("File ID must be positive");
    }
    return number;
}

void printFile(const raid::FileRecord& file) {
    std::cout << "File ID: file" << file.id << "\nOriginal name: " << file.name << "\nLUN: " << file.lun
              << "\nSize: " << file.size << " bytes\nStart LBA: " << file.startLba
              << "\nBlock count: " << file.blocks << "\nSHA-256: " << (file.sha256.empty() ? "-" : file.sha256)
              << "\nImport status: " << file.status << "\nCreated (UTC): " << file.created
              << "\nUpdated (UTC): " << file.updated << '\n';
    if (!file.importError.empty()) {
        std::cout << "Import error: " << file.importError << '\n';
    }
}

void printVerification(const raid::FileVerification& result) {
    std::cout << "File ID: file" << result.id << "\nFile: " << result.name << "\nSize: " << result.size
              << " bytes\nExpected SHA-256: " << result.expected
              << "\nActual SHA-256: " << (result.actual.empty() ? "-" : result.actual)
              << "\nIntegrity: " << (result.passed() ? "PASSED" : "FAILED") << '\n';
    if (!result.error.empty()) {
        std::cerr << "Error: " << result.error << '\n';
    }
}

}  // namespace

int runFileCommand(const std::vector<std::string>& args) {
    raid::FileManager manager(raid::config::projectRoot());
    if (args.size() == 4 && args[1] == "import") {
        printFile(manager.importFile(args[2], std::filesystem::u8path(args[3])));
    } else if ((args.size() == 2 || args.size() == 3) && args[1] == "list") {
        std::cout << "File ID | LUN | Bytes | Import status | Created (UTC) | SHA-256 prefix | Original name\n";
        for (const auto& file : manager.list(args.size() == 3 ? args[2] : "")) {
            std::cout << "file" << file.id << " | " << file.lun << " | " << file.size << " | " << file.status
                      << " | " << file.created << " | " << (file.sha256.empty() ? "-" : file.sha256.substr(0, 12))
                      << " | " << file.name << '\n';
        }
    } else if (args.size() == 3 && args[1] == "info") {
        printFile(manager.info(parseId(args[2])));
    } else if (args.size() == 3 && args[1] == "verify") {
        const auto result = manager.verify(parseId(args[2]));
        printVerification(result);
        return result.passed() ? 0 : 1;
    } else if (args.size() == 3 && args[1] == "verify-all") {
        bool passed = true;
        const auto results = manager.verifyAll(args[2]);
        for (const auto& result : results) {
            printVerification(result);
            passed = result.passed() && passed;
        }
        std::cout << "Completed files checked: " << results.size() << '\n';
        return passed ? 0 : 1;
    } else if (args.size() == 4 && args[1] == "export") {
        const auto result = manager.exportFile(parseId(args[2]), std::filesystem::u8path(args[3]));
        printVerification(result);
        if (result.passed()) {
            std::cout << "Exported file" << result.id << " (" << result.size << " bytes).\n";
        }
        return result.passed() ? 0 : 1;
    } else {
        throw std::invalid_argument("Use file import, list, info, export, verify, or verify-all. See --help.");
    }
    return 0;
}
