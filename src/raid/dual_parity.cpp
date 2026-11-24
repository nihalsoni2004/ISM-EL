#include "raid/dual_parity.h"

#include "raid/gf256.h"

#include <stdexcept>

namespace raid::dual_parity {

std::uint8_t coefficient(std::size_t dataUnitIndex) {
    if (dataUnitIndex >= maxRaid6Members - 2) {
        throw std::out_of_range("RAID 6 data unit exceeds the distinct GF(256) coefficient range");
    }
    return gf256::power(gf256::alpha, static_cast<unsigned>(dataUnitIndex));
}

void xorScaled(std::uint8_t* target, const std::uint8_t* source, std::size_t bytes, std::uint8_t weight) {
    if (!target || !source || bytes == 0 || weight == 0) {
        throw std::invalid_argument("Scaled XOR requires byte buffers and a nonzero coefficient");
    }
    for (std::size_t index = 0; index < bytes; ++index) {
        target[index] ^= gf256::multiply(source[index], weight);
    }
}

void accumulate(std::uint8_t* p, std::uint8_t* q, const std::uint8_t* data,
                std::size_t bytes, std::uint8_t weight) {
    if (!p || !q || !data || p == q || bytes == 0 || weight == 0) {
        throw std::invalid_argument("P/Q accumulation requires separate parity buffers and a nonzero coefficient");
    }
    for (std::size_t index = 0; index < bytes; ++index) {
        const auto value = data[index];
        p[index] ^= value;
        q[index] ^= gf256::multiply(value, weight);
    }
}

ParityBlock recoverFromQ(const ParityBlock& qResidual, std::uint8_t weight) {
    ParityBlock data{};
    xorScaled(data.data(), qResidual.data(), data.size(), gf256::inverse(weight));
    return data;
}

std::pair<ParityBlock, ParityBlock> recoverTwo(const ParityBlock& pResidual, const ParityBlock& qResidual,
                                               std::uint8_t firstWeight, std::uint8_t secondWeight) {
    if (firstWeight == 0 || secondWeight == 0 || firstWeight == secondWeight) {
        throw std::invalid_argument("Two-data recovery requires distinct nonzero coefficients");
    }

    const auto inverse = gf256::inverse(gf256::add(firstWeight, secondWeight));
    std::pair<ParityBlock, ParityBlock> recovered;
    for (std::size_t index = 0; index < pResidual.size(); ++index) {
        const auto numerator = gf256::add(qResidual[index], gf256::multiply(secondWeight, pResidual[index]));
        recovered.first[index] = gf256::multiply(numerator, inverse);
        recovered.second[index] = gf256::add(pResidual[index], recovered.first[index]);
    }
    return recovered;
}

}  // namespace raid::dual_parity
