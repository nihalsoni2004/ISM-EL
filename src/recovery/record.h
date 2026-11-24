#pragma once

#include "disk/image_file.h"

#include <charconv>
#include <chrono>
#include <sstream>

namespace raid::record {

inline std::string field(std::istream& input, std::string_view key) {
    std::string line;
    const auto prefix = std::string(key) + '=';
    if (!std::getline(input, line) || line.compare(0, prefix.size(), prefix) != 0) {
        throw std::invalid_argument("Expected metadata field " + std::string(key));
    }
    if (line.back() == '\r') {
        line.pop_back();
    }
    return line.substr(prefix.size());
}

inline std::uint64_t number(std::string_view text) {
    std::uint64_t value = 0;
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        throw std::invalid_argument("Invalid unsigned metadata number");
    }
    return value;
}

inline std::vector<std::string> split(std::string_view list) {
    std::vector<std::string> result;
    while (!list.empty()) {
        const auto comma = list.find(',');
        result.emplace_back(list.substr(0, comma));
        if (comma == std::string_view::npos) {
            return result;
        }
        list.remove_prefix(comma + 1);
        if (list.empty()) {
            throw std::invalid_argument("Invalid trailing comma in metadata");
        }
    }
    return result;
}

inline std::string join(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) {
            result += ',';
        }
        result += value;
    }
    return result;
}

inline void requireEnd(std::istream& input) {
    std::string extra;
    if (std::getline(input, extra)) {
        throw std::invalid_argument("Unexpected metadata fields");
    }
}

inline std::optional<std::string> read(const std::filesystem::path& path) {
    try {
        return disk_file::readText(path);
    } catch (const std::filesystem::filesystem_error& error) {
        if (error.code() != std::errc::no_such_file_or_directory) {
            throw;
        }
        return std::nullopt;
    }
}

inline std::vector<std::filesystem::path> entries(const std::filesystem::path& path) {
    try {
        return disk_file::listEntries(path);
    } catch (const std::filesystem::filesystem_error& error) {
        if (error.code() != std::errc::no_such_file_or_directory) {
            throw;
        }
        return {};
    }
}

inline bool hasExtension(const std::filesystem::path& path, std::wstring_view extension) {
    auto actual = path.extension().native();
    for (auto& ch : actual) {
        if (ch >= L'A' && ch <= L'Z') {
            ch += L'a' - L'A';
        }
    }
    return actual == extension;
}

inline bool identity(std::string_view value) noexcept {
    if (value.size() != 64) {
        return false;
    }
    for (const char ch : value) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

inline std::uint64_t now() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

}  // namespace raid::record
