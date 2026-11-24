#pragma once

#include "raid/raid_types.h"

#include <array>

namespace raid {

using ParityBlock = std::array<std::uint8_t, raidBlockSize>;

// XOR equal-length binary buffers in place. Buffers can contain one or more blocks.
void xorInto(std::uint8_t* target, std::size_t targetSize,
             const std::uint8_t* source, std::size_t sourceSize);

}  // namespace raid
