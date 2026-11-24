#pragma once

#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>

namespace test {

inline void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Error, typename Operation>
void rejects(Operation operation, const char* message) {
    try {
        operation();
    } catch (const Error&) {
        return;
    }
    throw std::runtime_error(message);
}

class TempDirectory {
public:
    TempDirectory() {
        std::random_device random;
        path_ = std::filesystem::current_path() / ("raid-test-" + std::to_string(random()) +
                                                    "-" + std::to_string(random()));
        require(std::filesystem::create_directory(path_), "Could not create isolated test directory");
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace test
