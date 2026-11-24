#include "raid/parity.h"

#include <stdexcept>

namespace raid {

void xorInto(std::uint8_t* target, std::size_t targetSize,
             const std::uint8_t* source, std::size_t sourceSize) {
    if (!target || !source || targetSize == 0 || targetSize != sourceSize) {
        throw std::invalid_argument("XOR requires nonempty buffers of equal size");
    }
    for (std::size_t index = 0; index < targetSize; ++index) {
        target[index] ^= source[index];
    }
}

}  // namespace raid
