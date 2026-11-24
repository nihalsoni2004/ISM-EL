#include "core/config.h"

#define NOMINMAX
#include <windows.h>

#include <stdexcept>
#include <system_error>

namespace raid::config {

const std::string_view defaultDataDirectory = "data/virtual_disks";
const std::string_view defaultMetadataDirectory = "data/metadata";
const std::string_view applicationVersion = "0.1.0";

std::filesystem::path projectRoot() {
    namespace fs = std::filesystem;
    std::wstring executable(MAX_PATH, L'\0');
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (length == 0) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Locate simulator executable");
        }
        if (length < executable.size()) {
            executable.resize(length);
            break;
        }
        executable.resize(executable.size() * 2);
    }

    auto directory = fs::path(executable).parent_path();
    for (;;) {
        if (fs::is_regular_file(directory / "CMakePresets.json") &&
            fs::is_regular_file(directory / "CMakeLists.txt")) {
            return directory;
        }
        const auto parent = directory.parent_path();
        if (parent == directory || parent.empty()) {
            throw std::runtime_error("Cannot locate the project root above the simulator executable");
        }
        directory = parent;
    }
}

}  // namespace raid::config
