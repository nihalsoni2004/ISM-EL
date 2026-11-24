#include "files/file_manager.h"

#include "disk/image_file.h"
#include "files/sha256.h"
#include "metadata/file_repository.h"
#include "provisioning/catalog.h"
#include "telemetry/telemetry.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace raid {
namespace {

using metadata::FileRepository;
using metadata::fileBlocks;
namespace fs = std::filesystem;

using Buffer = std::array<std::uint8_t, 64 * 1024>;

void requireLunId(std::string_view lun) {
    if (!provisioning::validId(lun, "lun")) {
        throw std::invalid_argument("Invalid LUN identifier. Use lun0, lun1, and so on.");
    }
}

std::string hashStored(ProvisioningManager& volumes, const FileRecord& file,
                       const disk_file::BinarySink& sink = {}) {
    Buffer buffer;
    Sha256 hash;
    std::uint64_t read = 0;
    while (read < file.size) {
        const auto bytes = static_cast<std::uint32_t>(std::min<std::uint64_t>(buffer.size(), file.size - read));
        volumes.readBlocks(file.lun, file.startLba + read / raidBlockSize,
                           fileBlocks(bytes), buffer.data(), buffer.size());
        hash.update(buffer.data(), bytes);
        if (sink) {
            sink(buffer.data(), bytes);
        }
        read += bytes;
    }
    return hash.finish();
}

fs::path hostPath(const fs::path& path) {
    if (path.empty() || path.filename().empty()) {
        throw std::invalid_argument("Expected a host file path");
    }
    return fs::absolute(path).lexically_normal();
}

}  // namespace

FileManager::FileManager(std::filesystem::path root, Observer observer)
    : root_(std::move(root)), observer_(std::move(observer)) {
    if (!root_.is_absolute() || !fs::is_directory(root_)) {
        throw std::invalid_argument("Storage root must be an existing absolute directory");
    }
}

LunInfo FileManager::createLun(std::string_view lun, std::string_view pool, std::uint64_t capacityBytes) {
    requireLunId(lun);
    FileRepository repo(root_);
    return ProvisioningManager(root_).createLunImpl(lun, pool, capacityBytes, repo.identity());
}

FileRecord FileManager::importFile(std::string_view lun, const fs::path& source) {
    requireLunId(lun);
    FileRepository repo(root_);
    ProvisioningManager volumes(root_);
    const auto volume = volumes.fileLun(lun, repo.identity(), true);
    const auto path = hostPath(source);
    disk_file::BinaryInput input(path, root_ / "data");
    const auto reserved = repo.reserve(volume, path.filename().u8string(), input.size());
    const auto fileId = reserved.operation.file;

    try {
        if (observer_) {
            observer_(ImportPhase::Reserved);
        }
        repo.transitionImport(fileId, "PENDING", "WRITING");
        Buffer buffer;
        Sha256 hash;
        std::uint64_t offset = 0;
        while (offset < input.size()) {
            const auto bytes = static_cast<std::uint32_t>(std::min<std::uint64_t>(buffer.size(), input.size() - offset));
            input.read(buffer.data(), bytes);
            hash.update(buffer.data(), bytes);
            std::fill(buffer.begin() + bytes, buffer.end(), std::uint8_t{0});
            const auto blocks = fileBlocks(bytes);
            volumes.writeBlocksImpl(lun, reserved.startLba + offset / raidBlockSize, blocks,
                                    buffer.data(), buffer.size(), repo.identity());
            offset += bytes;
            if (observer_) {
                observer_(ImportPhase::ChunkWritten);
            }
        }
        input.requireEof();
        auto expected = hash.finish();
        repo.transitionImport(fileId, "WRITING", "VERIFYING");
        const auto file = repo.find(fileId);
        volumes.fileLun(lun, repo.identity(), false);
        if (hashStored(volumes, file) != expected) {
            throw std::runtime_error("SHA-256 mismatch on import readback");
        }
        if (observer_) {
            observer_(ImportPhase::BeforeCompletion);
        }
        repo.finishImport(reserved.operation, expected);
    } catch (const std::exception& error) {
        const auto message = "Import file" + std::to_string(fileId) + " failed: " + error.what();
        try {
            repo.failImport(reserved.operation, error.what());
        } catch (const std::exception& metadataError) {
            throw std::runtime_error(message + ". Could not record failure: " + metadataError.what());
        }
        throw std::runtime_error(message);
    }
    return repo.find(fileId);
}

FileRecord FileManager::info(std::uint64_t id) {
    return FileRepository(root_).find(id);
}

std::vector<FileRecord> FileManager::list(std::string_view lun) {
    FileRepository repo(root_);
    if (!lun.empty()) {
        requireLunId(lun);
        ProvisioningManager(root_).fileLun(lun, repo.identity(), std::nullopt);
    }
    return repo.list(lun);
}

FileVerification FileManager::inspect(std::uint64_t id, const fs::path* destination) {
    FileRepository repo(root_);
    const auto file = repo.find(id);
    if (file.status != "COMPLETED") {
        throw std::invalid_argument("File import is " + file.status + ". Only completed files can be read");
    }
    ProvisioningManager volumes(root_);
    FileVerification result{id, file.name, file.size, file.sha256, {}, {}};
    const auto operation = repo.start(id, destination ? "EXPORT" : "VERIFY");
    try {
        const auto volume = volumes.fileLun(file.lun, repo.identity(), false);
        if (file.startLba > volume.blocks || file.blocks > volume.blocks - file.startLba) {
            throw std::runtime_error("File extent is outside the backing LUN");
        }
        if (destination) {
            disk_file::publishBinary(hostPath(*destination), root_ / "data", [&](const auto& sink) {
                result.actual = hashStored(volumes, file, sink);
                if (result.actual != result.expected) {
                    throw std::runtime_error("SHA-256 mismatch. Export was withheld");
                }
            });
        } else {
            result.actual = hashStored(volumes, file);
            if (result.actual != result.expected) {
                result.error = "SHA-256 mismatch";
            }
        }
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    repo.finish(operation, result.passed() ? "COMPLETED" : "FAILED", result.error, result.actual);
    return result;
}

FileVerification FileManager::verify(std::uint64_t id) {
    const telemetry::Scope scope(telemetry::Origin::Internal);
    return inspect(id, nullptr);
}

std::vector<FileVerification> FileManager::verifyAll(std::string_view lun) {
    const telemetry::Scope scope(telemetry::Origin::Internal);
    requireLunId(lun);
    std::vector<FileVerification> results;
    for (const auto& file : list(lun)) {
        if (file.status == "COMPLETED") {
            try {
                results.push_back(verify(file.id));
            } catch (const std::exception& error) {
                results.push_back({file.id, file.name, file.size, file.sha256, {}, error.what()});
            }
        }
    }
    return results;
}

FileVerification FileManager::exportFile(std::uint64_t id, const fs::path& destination) {
    return inspect(id, &destination);
}

}  // namespace raid
