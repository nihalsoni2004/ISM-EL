#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace raid::disk_file {

class InvalidImage : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

void prepareDirectory(const std::filesystem::path& path, bool create);
std::vector<std::filesystem::path> listEntries(const std::filesystem::path& path);
std::uint64_t inspect(const std::filesystem::path& path, std::string* identity = nullptr,
                     std::uint64_t* revision = nullptr);
std::string imageIdentity(std::FILE* stream);
std::uint64_t imageRevision(std::FILE* stream);
bool imagePresent(const std::filesystem::path& path);
std::FILE* open(const std::filesystem::path& path, std::uint64_t& capacityBytes);
void create(const std::filesystem::path& path, std::uint64_t capacityBytes);

// Use the same path validation, single-link checks and no-replace publication as images.
inline constexpr std::uint32_t defaultTextLimit = 64 * 1024;
std::optional<std::string> readText(const std::filesystem::path& path,
                                    std::uint32_t limit = defaultTextLimit);
std::optional<std::string> textFingerprint(const std::filesystem::path& path);
void publishText(const std::filesystem::path& path, std::string_view text,
                 std::uint32_t limit = defaultTextLimit);
void replaceText(const std::filesystem::path& path, std::string_view text,
                 std::uint32_t limit = defaultTextLimit);
void removeText(const std::filesystem::path& path, std::string_view expected);
std::shared_ptr<void> lockCatalog(const std::filesystem::path& path, bool wait = true);

// Guards the directory tree and pins the primary database file against replacement.
std::shared_ptr<void> pinDatabase(const std::filesystem::path& path);

class BinaryInput {
public:
    BinaryInput(const std::filesystem::path& path, const std::filesystem::path& protectedDirectory);
    std::uint64_t size() const noexcept { return size_; }
    void read(void* data, std::uint32_t size);
    void requireEof();

private:
    std::filesystem::path path_;
    std::shared_ptr<void> handle_;
    std::uint64_t size_;
};

using BinarySink = std::function<void(const void*, std::uint32_t)>;
void publishBinary(const std::filesystem::path& path, const std::filesystem::path& protectedDirectory,
                   const std::function<void(const BinarySink&)>& emit);

}  // namespace raid::disk_file
