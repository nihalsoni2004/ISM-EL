#pragma once

#include <cstdint>

namespace raid::gf256 {

inline constexpr unsigned polynomial = 0x11D;
inline constexpr std::uint8_t alpha = 0x02;

constexpr std::uint8_t add(std::uint8_t a, std::uint8_t b) noexcept { return a ^ b; }
std::uint8_t multiply(std::uint8_t a, std::uint8_t b) noexcept;
std::uint8_t inverse(std::uint8_t value);
std::uint8_t divide(std::uint8_t numerator, std::uint8_t denominator);
std::uint8_t power(std::uint8_t base, unsigned exponent) noexcept;

}  // namespace raid::gf256
