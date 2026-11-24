#pragma once

#include "files/file_manager.h"
#include "metadata/sqlite_database.h"

namespace raid::metadata {

inline constexpr std::uint64_t fileBlocks(std::uint64_t bytes) {
    return bytes / raidBlockSize + (bytes % raidBlockSize != 0);
}

struct FileOperation {
    std::uint64_t id;
    std::uint64_t file;
    std::shared_ptr<void> lease;
};

struct ImportReservation {
    FileOperation operation;
    std::uint64_t startLba;
};

class FileRepository {
public:
    explicit FileRepository(const std::filesystem::path& root);
    const std::string& identity() const { return identity_; }
    ImportReservation reserve(const LunInfo& lun, std::string_view name, std::uint64_t bytes);
    FileRecord find(std::uint64_t id);
    std::vector<FileRecord> list(std::string_view lun);
    FileOperation start(std::uint64_t file, std::string_view type);
    void transitionImport(std::uint64_t file, std::string_view before, std::string_view after);
    void finishImport(const FileOperation& operation, std::string_view hash);
    void failImport(const FileOperation& operation, std::string_view error);
    void finish(const FileOperation& operation, std::string_view state, std::string_view error,
                std::string_view hash = {});

private:
    FileOperation insertOperation(std::uint64_t file, std::string_view type);
    std::shared_ptr<void> tryLease(std::uint64_t id);
    void recover();
    void markImportFailed(std::uint64_t file);
    void finishOperation(std::uint64_t id, std::string_view state, std::string_view error,
                         std::string_view hash);
    std::filesystem::path leases_;
    Database db_;
    std::string identity_;
};

}  // namespace raid::metadata
