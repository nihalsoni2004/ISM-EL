#include "raid/gf256.h"
#include "test_support.h"

#include <array>
#include <iostream>
#include <limits>

namespace {

// Multiply the polynomials first, then reduce the product by long division.
unsigned referenceMultiply(unsigned a, unsigned b) {
    unsigned product = 0;
    for (unsigned bit = 0; bit < 8; ++bit) {
        if ((b >> bit) & 1U) {
            product ^= a << bit;
        }
    }
    for (int bit = 14; bit >= 8; --bit) {
        if ((product >> bit) & 1U) {
            product ^= 0x11DU << (bit - 8);
        }
    }
    return product;
}

}  // namespace

int main() {
    try {
        using namespace raid::gf256;
        using test::require;
        using Byte = std::uint8_t;
        require(multiply(0x80, 0x02) == 0x1D && multiply(0xFF, 0x02) == 0xE3 &&
                multiply(0x57, 0x83) == 0x31 && power(alpha, 8) == 0x1D, "known polynomial vectors");
        require(power(0, 0) == 1 && power(0, 9) == 0 && power(0xC7, 0) == 1, "power identity");
        require(power(alpha, std::numeric_limits<unsigned>::max()) ==
                power(alpha, std::numeric_limits<unsigned>::max() % 255), "large exponent overflow");

        std::array<bool, 256> seen{};
        Byte element = 1;
        for (unsigned index = 0; index < 255; ++index) {
            require(element != 0 && !seen[element] && element == power(alpha, index), "primitive element repeated");
            seen[element] = true;
            element = multiply(element, alpha);
        }
        require(element == 1 && power(alpha, 255) == 1, "wrong primitive element period");

        for (unsigned i = 0; i < 256; ++i) {
            const auto a = static_cast<Byte>(i);
            require(multiply(a, 0) == 0 && multiply(a, 1) == a, "zero or identity multiplication");
            if (a != 0) {
                require(multiply(a, inverse(a)) == 1 && referenceMultiply(a, inverse(a)) == 1,
                        "incorrect nonzero inverse");
            }
            for (unsigned j = 0; j < 256; ++j) {
                const auto b = static_cast<Byte>(j);
                require(add(a, b) == (a ^ b), "addition differs from XOR");
                require(multiply(a, b) == referenceMultiply(a, b) && multiply(a, b) == multiply(b, a),
                        "incorrect polynomial product");
                if (b != 0) {
                    require(multiply(divide(a, b), b) == a, "incorrect division");
                }
                for (auto c : std::array<Byte, 4>{0, 1, 0x97, static_cast<Byte>(i * 19 + j * 71)}) {
                    require(multiply(multiply(a, b), c) == multiply(a, multiply(b, c)), "associativity");
                    require(multiply(a, add(b, c)) == add(multiply(a, b), multiply(a, c)), "distributivity");
                }
            }
        }
        test::rejects<std::invalid_argument>([] { inverse(0); }, "inverse of zero accepted");
        test::rejects<std::invalid_argument>([] { divide(0, 0); }, "division by zero accepted");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
