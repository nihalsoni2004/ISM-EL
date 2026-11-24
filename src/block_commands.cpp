#include "block_commands.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace cli {

std::uint64_t parseNumber(std::string_view text, const char* label) {
    std::uint64_t value = 0;
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        throw std::invalid_argument(std::string(label) + " must be an unsigned decimal integer");
    }
    return value;
}

std::string capacityLabel(std::uint64_t bytes) {
    if (bytes % bytesPerMiB == 0) {
        return std::to_string(bytes / bytesPerMiB) + " MiB";
    }
    return std::to_string(bytes) + " bytes";
}

static std::string printableUtf8(std::string_view bytes) {
    std::string text;
    while (!bytes.empty()) {
        const auto first = static_cast<unsigned char>(bytes.front());
        if (first < 0x80) {
            text.push_back(first >= 0x20 && first < 0x7F ? bytes.front() : '.');
            bytes.remove_prefix(1);
            continue;
        }

        const int length = first < 0xC2 || first > 0xF4 ? 0 :
                           first < 0xE0 ? 2 : first < 0xF0 ? 3 : 4;
        wchar_t characters[2]{};
        if (length == 0 || static_cast<std::size_t>(length) > bytes.size() ||
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), length, characters, 2) == 0) {
            text.push_back('.');
            bytes.remove_prefix(1);
            continue;
        }

        const auto firstCharacter = characters[0];
        if (firstCharacter < 0xA0 || (firstCharacter >= 0x200B && firstCharacter <= 0x200F) ||
            (firstCharacter >= 0x2028 && firstCharacter <= 0x202E) ||
            (firstCharacter >= 0x2060 && firstCharacter <= 0x206F)) {
            text.push_back('.');
        } else {
            text.append(bytes.substr(0, length));
        }
        bytes.remove_prefix(length);
    }
    return text;
}

void printBlock(std::string_view id, std::uint64_t lba, const Block& block) {
    constexpr std::size_t previewLimit = 64;
    const auto lastUsed = std::find_if(block.rbegin(), block.rend(), [](auto byte) { return byte != 0; });
    const auto used = static_cast<std::size_t>(lastUsed.base() - block.begin());
    const auto shown = std::min(used, previewLimit);

    std::cout << id << " LBA " << lba << '\n';
    if (used == 0) {
        std::cout << "Block is all zeros (" << block.size() << " bytes).\n";
        return;
    }

    std::cout << "Hex:" << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < shown; ++index) {
        std::cout << ' ' << std::setw(2) << static_cast<unsigned int>(block[index]);
    }
    std::cout << std::dec << std::setfill(' ') << "\nText: "
              << printableUtf8({reinterpret_cast<const char*>(block.data()), shown}) << '\n';
    if (shown < used) {
        std::cout << "Preview truncated after " << shown << " of " << used << " bytes before trailing zeros.\n";
    }
    if (used < block.size()) {
        std::cout << block.size() - used << " trailing zero bytes omitted.\n";
    }
}

}  // namespace cli
