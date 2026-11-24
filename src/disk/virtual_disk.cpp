#include "disk/virtual_disk.h"

#include "core/config.h"
#include "disk/virtual_disk_manager.h"
#include "image_file.h"

#include <cerrno>
#include <io.h>
#include <limits>
#include <system_error>

namespace raid {

namespace {

[[noreturn]] void failStream(const char* action) {
    throw std::system_error(errno, std::generic_category(), action);
}

}  // namespace

void VirtualDisk::create(const std::filesystem::path& path, std::uint64_t capacityBytes) {
    if (isOpen()) {
        throw std::logic_error("Close the disk before creating another image");
    }
    if (capacityBytes == 0 || capacityBytes % getBlockSize() != 0 ||
        capacityBytes > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        throw std::invalid_argument("Capacity must be a positive, representable multiple of 4096 bytes");
    }

    disk_file::create(path, capacityBytes);
    open(path);
}

void VirtualDisk::open(const std::filesystem::path& path) {
    if (isOpen()) {
        throw std::logic_error("Close the disk before opening another image");
    }
    file_.reset(disk_file::open(path, capacityBytes_));
    telemetryId_ = telemetry::Identifier{};
    try {
        const auto id = path.stem().u8string();
        if (VirtualDiskManager::isValidDiskId(id)) telemetryId_ = telemetry::Identifier(id);
    } catch (...) {
        // Optional context must not prevent an otherwise valid image from opening.
    }
}

void VirtualDisk::close() {
    if (!isOpen()) {
        return;
    }

    try {
        flush();
    } catch (...) {
        file_.reset();
        capacityBytes_ = 0;
        dirty_ = false;
        throw;
    }

    capacityBytes_ = 0;
    if (std::fclose(file_.release()) != 0) {
        failStream("Close disk image");
    }
}

void VirtualDisk::flush() {
    if (!isOpen()) {
        throw std::logic_error("Disk is closed");
    }
    if (dirty_) {
        if (std::fflush(file_.get()) != 0 || _commit(_fileno(file_.get())) != 0) {
            failStream("Flush disk image");
        }
        dirty_ = false;
    }
}

std::size_t VirtualDisk::seekToBlocks(std::uint64_t lba, std::uint64_t blockCount,
                                      const void* buffer, std::size_t bufferSize) {
    if (!isOpen()) {
        throw std::logic_error("Disk is closed");
    }
    if (blockCount == 0 || buffer == nullptr) {
        throw std::invalid_argument("Block count and buffer must be nonzero");
    }
    const auto totalBlocks = getBlockCount();
    if (lba >= totalBlocks || blockCount > totalBlocks - lba) {
        throw std::out_of_range("Requested block range is outside the disk");
    }

    // The validated range is within an aligned capacity representable by a signed seek.
    const auto length = blockCount * getBlockSize();
    if (length > bufferSize) {
        throw std::invalid_argument("Buffer is too small for the requested blocks");
    }

    std::clearerr(file_.get());
    if (_fseeki64(file_.get(), static_cast<std::int64_t>(lba * getBlockSize()), SEEK_SET) != 0) {
        failStream("Seek in disk image");
    }
    return static_cast<std::size_t>(length);
}

void VirtualDisk::readBlocks(std::uint64_t lba, std::uint64_t blockCount,
                             std::uint8_t* buffer, std::size_t bufferSize) {
    telemetry::OperationTimer timer(telemetry::Kind::DiskRead, blockCount, getBlockSize(), telemetryId_);
    const auto bytes = seekToBlocks(lba, blockCount, buffer, bufferSize);
    if (std::fread(buffer, 1, bytes, file_.get()) != bytes) {
        throw std::runtime_error("Short read or I/O failure in disk image");
    }
    timer.success();
}

void VirtualDisk::writeBlocks(std::uint64_t lba, std::uint64_t blockCount,
                              const std::uint8_t* buffer, std::size_t bufferSize) {
    telemetry::OperationTimer timer(telemetry::Kind::DiskWrite, blockCount, getBlockSize(), telemetryId_);
    const auto bytes = seekToBlocks(lba, blockCount, buffer, bufferSize);
    dirty_ = true;
    if (std::fwrite(buffer, 1, bytes, file_.get()) != bytes) {
        throw std::runtime_error("Short write or I/O failure in disk image");
    }
    timer.success();
}

bool VirtualDisk::isOpen() const noexcept {
    return file_ != nullptr;
}

std::uint64_t VirtualDisk::getCapacityBytes() const noexcept {
    return capacityBytes_;
}

std::uint64_t VirtualDisk::getBlockCount() const noexcept {
    return capacityBytes_ / getBlockSize();
}

std::uint64_t VirtualDisk::getBlockSize() const noexcept {
    return config::defaultLogicalBlockSizeBytes;
}

std::string VirtualDisk::imageIdentity() const {
    if (!isOpen()) {
        throw std::logic_error("Disk is closed");
    }
    return disk_file::imageIdentity(file_.get());
}

std::uint64_t VirtualDisk::imageRevision() const {
    if (!isOpen()) {
        throw std::logic_error("Disk is closed");
    }
    return disk_file::imageRevision(file_.get());
}

}  // namespace raid
