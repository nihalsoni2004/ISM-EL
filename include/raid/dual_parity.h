#pragma once

#include "raid/parity.h"

#include <utility>

namespace raid::dual_parity {

std::uint8_t coefficient(std::size_t dataUnitIndex);
void xorScaled(std::uint8_t* target, const std::uint8_t* source,
               std::size_t bytes, std::uint8_t weight);
void accumulate(std::uint8_t* p, std::uint8_t* q, const std::uint8_t* data,
                std::size_t bytes, std::uint8_t weight);

// Residuals contain stored parity with contributions from surviving data removed.
ParityBlock recoverFromQ(const ParityBlock& qResidual, std::uint8_t weight);
std::pair<ParityBlock, ParityBlock> recoverTwo(const ParityBlock& pResidual, const ParityBlock& qResidual,
                                               std::uint8_t firstWeight, std::uint8_t secondWeight);

}  // namespace raid::dual_parity
