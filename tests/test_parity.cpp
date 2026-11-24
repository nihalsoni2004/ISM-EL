#include "raid/parity.h"
#include "test_support.h"

#include <iostream>

int main() {
    try {
        using raid::ParityBlock;
        using test::require;
        const ParityBlock zeros{};
        ParityBlock ff{};
        ff.fill(0xFF);

        auto result = zeros;
        const auto xorBlock = [](ParityBlock& target, const ParityBlock& source) {
            raid::xorInto(target.data(), target.size(), source.data(), source.size());
        };
        xorBlock(result, zeros);
        require(result == zeros, "zero XOR zero failed");
        xorBlock(result, ff);
        require(result == ff, "zero XOR FF failed");
        xorBlock(result, ff);
        require(result == zeros, "FF XOR FF failed");

        std::array<ParityBlock, 3> inputs{};
        std::uint32_t seed = 0xABCDEF;
        for (std::size_t index = 0; index < result.size(); ++index) {
            seed = seed * 1664525U + 1013904223U;
            inputs[0][index] = static_cast<std::uint8_t>(index);
            inputs[1][index] = static_cast<std::uint8_t>(seed >> 24);
            inputs[2][index] = index % 3 == 0 ? 0 : 0xA3;
        }
        for (const auto& input : inputs) {
            xorBlock(result, input);
        }
        for (std::size_t index = 0; index < result.size(); ++index) {
            require(result[index] == (inputs[0][index] ^ inputs[1][index] ^ inputs[2][index]),
                    "binary parity byte differs");
        }
        for (std::size_t missing = 0; missing < inputs.size(); ++missing) {
            auto reconstructed = result;
            for (std::size_t index = 0; index < inputs.size(); ++index) {
                if (index != missing) {
                    xorBlock(reconstructed, inputs[index]);
                }
            }
            require(reconstructed == inputs[missing], "binary reconstruction lost a byte");
        }

        test::rejects<std::invalid_argument>([&] {
            raid::xorInto(result.data(), result.size(), result.data(), result.size() - 1);
        }, "XOR accepted mismatched buffers");
        test::rejects<std::invalid_argument>([&] {
            raid::xorInto(nullptr, result.size(), result.data(), result.size());
        }, "XOR accepted null buffer");
        test::rejects<std::invalid_argument>([&] {
            raid::xorInto(result.data(), 0, result.data(), 0);
        }, "XOR accepted empty buffers");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
