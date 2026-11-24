#include "raid/gf256.h"

#include <array>
#include <stdexcept>

namespace raid::gf256 {
namespace {

constexpr unsigned period = 255;

struct Tables {
    std::array<std::uint8_t, 2 * period> exponent{};
    std::array<unsigned, 256> logarithm{};

    constexpr Tables() {
        unsigned value = 1;
        for (unsigned index = 0; index < period; ++index) {
            exponent[index] = static_cast<std::uint8_t>(value);
            logarithm[value] = index;
            value <<= 1;
            if (value & 0x100U) {
                value ^= polynomial;
            }
        }
        // Repeat the cycle so multiplication needs no modulo operation.
        for (unsigned index = period; index < exponent.size(); ++index) {
            exponent[index] = exponent[index - period];
        }
    }
};

constexpr Tables tables;

}  // namespace

std::uint8_t multiply(std::uint8_t a, std::uint8_t b) noexcept {
    if (a == 0 || b == 0) {
        return 0;
    }
    return tables.exponent[tables.logarithm[a] + tables.logarithm[b]];
}

std::uint8_t inverse(std::uint8_t value) {
    if (value == 0) {
        throw std::invalid_argument("Zero has no GF(256) inverse");
    }
    return tables.exponent[period - tables.logarithm[value]];
}

std::uint8_t divide(std::uint8_t numerator, std::uint8_t denominator) {
    return multiply(numerator, inverse(denominator));
}

std::uint8_t power(std::uint8_t base, unsigned exponent) noexcept {
    if (exponent == 0) {
        return 1;
    }
    if (base == 0) {
        return 0;
    }
    return tables.exponent[(tables.logarithm[base] * (exponent % period)) % period];
}

}  // namespace raid::gf256
