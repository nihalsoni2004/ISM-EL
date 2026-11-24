#pragma once

#include "telemetry/telemetry.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace raid {

class VirtualDisk {
public:
    VirtualDisk() = default;
    ~VirtualDisk() = default;

    VirtualDisk(const VirtualDisk&) = delete;
    VirtualDisk& operator=(const VirtualDisk&) = delete;

    void create(const std::filesystem::path& path, std::uint64_t capacityBytes);
    void open(const std::filesystem::path& path);
    void close();  // Idempotent. Releases the file even if flushing fails.
    void flush();

    void readBlocks(std::uint64_t lba, std::uint64_t blockCount,
                    std::uint8_t* buffer, std::size_t bufferSize);
    void writeBlocks(std::uint64_t lba, std::uint64_t blockCount,
                     const std::uint8_t* buffer, std::size_t bufferSize);

    bool isOpen() const noexcept;
    std::uint64_t getCapacityBytes() const noexcept;
    std::uint64_t getBlockCount() const noexcept;
    std::uint64_t getBlockSize() const noexcept;
    std::string imageIdentity() const;
    std::uint64_t imageRevision() const;

private:
    std::size_t seekToBlocks(std::uint64_t lba, std::uint64_t blockCount,
                             const void* buffer, std::size_t bufferSize);

    std::unique_ptr<std::FILE, decltype(&std::fclose)> file_{nullptr, &std::fclose};
    std::uint64_t capacityBytes_ = 0;
    bool dirty_ = false;
    telemetry::Identifier telemetryId_;
};

}  // namespace raid
