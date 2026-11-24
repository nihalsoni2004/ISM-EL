#include "raid/dual_parity.h"
#include "test_support.h"

#include <iostream>

namespace {

using raid::ParityBlock;
using namespace raid::dual_parity;
using test::require;

constexpr std::size_t units = 4;
using Data = std::array<ParityBlock, units>;

std::pair<ParityBlock, ParityBlock> encode(const Data& data) {
    ParityBlock p{}, q{};
    for (std::size_t index = 0; index < data.size(); ++index) {
        accumulate(p.data(), q.data(), data[index].data(), p.size(), coefficient(index));
    }
    return {p, q};
}

void checkRecovery(const Data& data) {
    const auto [p, q] = encode(data);
    for (std::size_t first = 0; first < units; ++first) {
        auto pResidual = p;
        auto qResidual = q;
        for (std::size_t survivor = 0; survivor < units; ++survivor) {
            if (survivor != first) {
                accumulate(pResidual.data(), qResidual.data(), data[survivor].data(),
                           p.size(), coefficient(survivor));
            }
        }
        require(pResidual == data[first], "XOR recovery failed");
        require(recoverFromQ(qResidual, coefficient(first)) == data[first], "Q-only recovery failed");

        for (std::size_t second = first + 1; second < units; ++second) {
            // Restore the second contribution, leaving both missing units in the residuals.
            auto bothP = pResidual;
            auto bothQ = qResidual;
            accumulate(bothP.data(), bothQ.data(), data[second].data(), p.size(), coefficient(second));
            const auto recovered = recoverTwo(bothP, bothQ, coefficient(first), coefficient(second));
            require(recovered.first == data[first] && recovered.second == data[second], "two-data recovery failed");
        }
    }
}

}  // namespace

int main() {
    try {
        Data data{};
        require(encode(data) == std::make_pair(ParityBlock{}, ParityBlock{}), "nonzero parity for blank data");
        checkRecovery(data);
        for (auto& block : data) {
            block.fill(0xFF);
        }
        checkRecovery(data);

        std::uint32_t seed = 0xA512;
        for (std::size_t index = 0; index < data[0].size(); ++index) {
            seed = seed * 1664525U + 1013904223U;
            data[0][index] = static_cast<std::uint8_t>(index);
            data[1][index] = static_cast<std::uint8_t>(seed >> 24);
            data[2][index] = std::uint8_t{0x80};
            data[3][index] = index % 5 == 0 ? std::uint8_t{0} : std::uint8_t{0xFF};
        }
        // Independent hand-calculated vector for weights 1, 2, 4, 8.
        data[0][0] = 0xFF;
        data[1][0] = 0xAA;
        data[2][0] = 0x10;
        data[3][0] = 0;
        const auto [p, q] = encode(data);
        require(p[0] == 0x45 && q[0] == 0xF6, "independent P/Q vector differs");
        checkRecovery(data);

        std::array<bool, 256> weights{};
        for (std::size_t index = 0; index < 255; ++index) {
            const auto weight = coefficient(index);
            require(weight != 0 && !weights[weight], "Q coefficient repeated");
            weights[weight] = true;
        }
        test::rejects<std::out_of_range>([] { coefficient(255); }, "repeating coefficient accepted");
        test::rejects<std::invalid_argument>([&] { recoverTwo(p, q, 2, 2); }, "singular recovery accepted");
        test::rejects<std::invalid_argument>([&] { recoverTwo(p, q, 1, 0); }, "zero recovery weight accepted");
        test::rejects<std::invalid_argument>([&] { recoverFromQ(q, 0); }, "zero Q weight accepted");
        test::rejects<std::invalid_argument>([&] {
            xorScaled(nullptr, data[0].data(), data[0].size(), 1);
        }, "missing scaled XOR buffer accepted");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
