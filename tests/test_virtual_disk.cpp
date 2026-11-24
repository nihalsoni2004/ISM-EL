#include "core/config.h"
#include "disk/virtual_disk.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using test::rejects;
using test::require;

constexpr std::uint64_t capacity = 1024 * 1024;
constexpr auto blockSize = raid::config::defaultLogicalBlockSizeBytes;
using Block = std::array<std::uint8_t, blockSize>;

struct Fixture {
    test::TempDirectory directory;
    fs::path image = directory.path() / "disk0.img";
    raid::VirtualDisk disk;

    Fixture() { disk.create(image, capacity); }
};

Block read(raid::VirtualDisk& disk, std::uint64_t lba) {
    Block result{};
    disk.readBlocks(lba, 1, result.data(), result.size());
    return result;
}

void testCapacity() {
    Fixture fixture;
    auto& disk = fixture.disk;
    require(disk.isOpen(), "creation opens the image");
    require(fs::file_size(fixture.image) == capacity, "exact image size");
    require(disk.getCapacityBytes() == capacity && disk.getBlockCount() == 256 &&
            disk.getBlockSize() == blockSize, "reported disk geometry");
    require(read(disk, 0) == Block{} && read(disk, 127) == Block{} && read(disk, 255) == Block{},
            "untouched first, middle, and last blocks read zero");
}

void testIoAndPersistence() {
    Fixture fixture;
    auto& disk = fixture.disk;
    Block ff{};
    ff.fill(0xFF);
    disk.writeBlocks(10, 1, ff.data(), ff.size());
    require(read(disk, 10) == ff, "single-block write and read");

    std::array<std::uint8_t, 3 * blockSize> pattern{};
    std::uint32_t seed = 12345;
    for (auto& byte : pattern) {
        seed = seed * 1664525U + 1013904223U;
        byte = static_cast<std::uint8_t>(seed >> 24);
    }
    disk.writeBlocks(11, 3, pattern.data(), pattern.size());
    require(read(disk, 10) == ff && read(disk, 14) == Block{}, "neighbor blocks were preserved");
    disk.writeBlocks(0, 1, ff.data(), ff.size());
    disk.writeBlocks(255, 1, ff.data(), ff.size());
    disk.flush();

    // Verify flush while the original handle remains open.
    std::ifstream observer(fixture.image, std::ios::binary);
    observer.seekg(10 * blockSize);
    Block observed{};
    observer.read(reinterpret_cast<char*>(observed.data()), observed.size());
    require(observer.good() && observed == ff, "explicit flush makes data visible");
    observer.close();

    // An additional write must also be persisted by close alone.
    Block incrementing{};
    for (std::size_t index = 0; index < incrementing.size(); ++index) {
        incrementing[index] = static_cast<std::uint8_t>(index);
    }
    disk.writeBlocks(15, 1, incrementing.data(), incrementing.size());
    disk.close();
    disk.close();
    require(!disk.isOpen() && disk.getCapacityBytes() == 0 && disk.getBlockCount() == 0,
            "idempotent close resets state");
    raid::VirtualDisk reopened;
    reopened.open(fixture.image);
    auto recovered = pattern;
    recovered.fill(0);
    reopened.readBlocks(11, 3, recovered.data(), recovered.size());
    require(recovered == pattern, "all bytes in three blocks survive reopen");
    require(read(reopened, 0) == ff && read(reopened, 255) == ff, "first and last blocks persist");
    require(read(reopened, 15) == incrementing, "close persists an unflushed write");
    require(read(reopened, 10) == ff && read(reopened, 14) == Block{}, "adjacent blocks persist unchanged");
}

void testRejectedIo() {
    Fixture fixture;
    auto& disk = fixture.disk;
    Block block{};
    const auto max = (std::numeric_limits<std::uint64_t>::max)();
    for (const auto& invalid : {std::array<std::uint64_t, 2>{256, 1}, {255, 2}, {max, 1}, {1, max}}) {
        rejects<std::out_of_range>([&] { disk.readBlocks(invalid[0], invalid[1], block.data(), block.size()); },
                                   "invalid read range accepted");
        rejects<std::out_of_range>([&] { disk.writeBlocks(invalid[0], invalid[1], block.data(), block.size()); },
                                   "invalid write range accepted");
    }
    rejects<std::invalid_argument>([&] { disk.readBlocks(0, 0, block.data(), block.size()); }, "zero read");
    rejects<std::invalid_argument>([&] { disk.writeBlocks(0, 0, block.data(), block.size()); }, "zero write");
    rejects<std::invalid_argument>([&] { disk.readBlocks(0, 1, nullptr, block.size()); }, "null read");
    rejects<std::invalid_argument>([&] { disk.writeBlocks(0, 1, nullptr, block.size()); }, "null write");
    rejects<std::invalid_argument>([&] { disk.readBlocks(0, 1, block.data(), block.size() - 1); }, "short read");
    rejects<std::invalid_argument>([&] { disk.writeBlocks(0, 1, block.data(), block.size() - 1); }, "short write");
    require(read(disk, 0) == Block{} && fs::file_size(fixture.image) == capacity,
            "rejected writes keep bytes and size unchanged");

    disk.close();
    rejects<std::logic_error>([&] { disk.readBlocks(0, 1, block.data(), block.size()); }, "closed read");
    rejects<std::logic_error>([&] { disk.writeBlocks(0, 1, block.data(), block.size()); }, "closed write");
    rejects<std::logic_error>([&] { disk.flush(); }, "closed flush");
}

void testRejectedImages() {
    Fixture fixture;
    raid::VirtualDisk disk;
    const auto dir = fixture.directory.path();
    const auto other = dir / "other.img";
    for (auto invalid : {std::uint64_t{0}, capacity + 1, std::uint64_t{1} << 63}) {
        rejects<std::invalid_argument>([&] { disk.create(other, invalid); }, "invalid capacity accepted");
    }
    require(!fs::exists(other), "invalid create left an image");
    rejects<fs::filesystem_error>([&] { disk.open(other); }, "nonexistent image opened");
    const auto empty = dir / "empty.img";
    const auto malformed = dir / "malformed.img";
    std::ofstream{empty, std::ios::binary};
    std::ofstream{malformed, std::ios::binary}.put('x');
    rejects<std::invalid_argument>([&] { disk.open(empty); }, "empty image opened");
    rejects<std::invalid_argument>([&] { disk.open(malformed); }, "unaligned image opened");
    rejects<std::invalid_argument>([&] { disk.create(fixture.image, 2 * capacity); }, "duplicate overwrote image");
    require(fs::file_size(fixture.image) == capacity, "duplicate creation preserves original size");
    rejects<fs::filesystem_error>([&] { disk.create(dir / "missing" / "disk1.img", capacity); },
                                  "creation under missing parent succeeded");
    rejects<std::invalid_argument>([&] { disk.create(dir / "wrong.txt", capacity); }, "invalid extension");
    require(!disk.isOpen(), "failed operations leave disk closed");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view test = argc == 2 ? argv[1] : "";
        if (test == "capacity") {
            testCapacity();
        } else if (test == "io") {
            testIoAndPersistence();
        } else if (test == "invalid-io") {
            testRejectedIo();
        } else if (test == "invalid-images") {
            testRejectedImages();
        } else {
            throw std::invalid_argument("Unknown virtual disk test");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
