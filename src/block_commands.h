#pragma once

#include "core/config.h"

#include <array>
#include <string>

namespace cli {

inline constexpr std::uint64_t bytesPerMiB = 1024 * 1024;
inline constexpr auto blockSize = raid::config::defaultLogicalBlockSizeBytes;
using Block = std::array<std::uint8_t, blockSize>;

std::uint64_t parseNumber(std::string_view text, const char* label);
std::string capacityLabel(std::uint64_t bytes);
void printBlock(std::string_view id, std::uint64_t lba, const Block& block);

}  // namespace cli
