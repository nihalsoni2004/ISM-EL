#pragma once

#include "core/engine.h"
#include "test_support.h"

#include <fstream>
#include <iterator>

namespace failure_test {

namespace fs = std::filesystem;
using test::require;
constexpr auto memberBlocks = std::uint64_t{16};

inline std::string readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.is_open(), "Could not open test file for reading");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

inline void writeFile(const fs::path& path, std::string_view bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    require(!file.fail(), "Could not write test fixture");
}

struct ArrayFixture {
    test::TempDirectory root;
    fs::path directory = root.path() / "data" / "metadata" / "raid_arrays";
    fs::path metadata = directory / "array0.raid";
    fs::path state = directory / "array0.state";
    fs::path pending = directory / "array0.pending";
    std::vector<std::string> ids;
    std::vector<std::uint8_t> payload;

    ArrayFixture(raid::RaidLevel level, std::size_t memberCount) {
        raid::StorageEngine writer;
        require(writer.initialize(root.path()), "Test engine initialization failed");
        for (std::size_t index = 0; index < memberCount; ++index) {
            ids.push_back("disk" + std::to_string(index));
            writer.disks().createDisk(ids.back(), memberBlocks * raid::raidBlockSize);
        }
        auto& array = writer.arrays().createArray("array0", level, ids, 1);
        payload.resize(array.capacityBlocks() * raid::raidBlockSize);
        std::uint32_t seed = 0xA531;
        for (auto& byte : payload) {
            seed = seed * 1664525U + 1013904223U;
            byte = static_cast<std::uint8_t>(seed >> 24);
        }
        array.writeBlocks(0, array.capacityBlocks(), payload.data(), payload.size());
        require(writer.shutdown(), "Test engine shutdown failed");
    }

    fs::path image(std::size_t index) const {
        return root.path() / "data" / "virtual_disks" / (ids[index] + ".img");
    }

    std::vector<std::string> snapshot() const {
        std::vector<std::string> result;
        for (std::size_t index = 0; index < ids.size(); ++index) {
            result.push_back(readFile(image(index)));
        }
        return result;
    }

    void expectRead(raid::RaidController& array) const {
        std::vector<std::uint8_t> actual(payload.size());
        array.readBlocks(0, array.capacityBlocks(), actual.data(), actual.size());
        require(actual == payload, "Persistent degraded read did not return the original binary payload");
    }
};

}  // namespace failure_test
