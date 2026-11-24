#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace raid::config {

// Sizes are in bytes. A MiB is 2^20 bytes, while an MB is 10^6 bytes.
inline constexpr std::uint32_t defaultLogicalBlockSizeBytes = 4096;
inline constexpr std::uint64_t defaultVirtualDiskSizeBytes = 100ULL * 1024 * 1024;
inline constexpr std::uint32_t defaultThinProvisioningChunkSizeBytes = 65536;

extern const std::string_view defaultDataDirectory;
extern const std::string_view defaultMetadataDirectory;
extern const std::string_view applicationVersion;
std::filesystem::path projectRoot();

}  // namespace raid::config
