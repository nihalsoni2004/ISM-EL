#include "metadata/file_repository.h"

#include "metadata/schema.h"
#include "provisioning/catalog.h"
#include "recovery/record.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace raid::metadata {
namespace {

constexpr auto maxBlocks = std::numeric_limits<std::uint64_t>::max() / raidBlockSize;
constexpr auto fileColumns = R"sql(
SELECT f.file_id, f.lun_id, f.original_name, f.file_size_bytes, e.lun_start_lba, e.block_count,
       coalesce(f.sha256,''), f.import_status, f.created_at, f.updated_at,
       coalesce((SELECT o.error_message FROM file_operations o
                 WHERE o.file_id=f.file_id AND o.operation_type='IMPORT'),''),
       e.file_offset_bytes, e.extent_order, f.format_version
FROM files f LEFT JOIN file_extents e ON e.file_id=f.file_id
)sql";

FileRecord readFile(Statement& row) {
    FileRecord file{row.number(0), row.text(1), row.text(2), row.number(3), row.number(4), row.number(5),
                    row.text(6), row.text(7), row.text(8), row.text(9), row.text(10)};
    const std::array<std::string_view, 5> states{"PENDING", "WRITING", "VERIFYING", "COMPLETED", "FAILED"};
    if (file.id == 0 || !provisioning::validId(file.lun, "lun") || file.name.empty() ||
        file.name.find_first_of("\\/:\r\n") != std::string::npos || file.name.find('\0') != std::string::npos ||
        file.startLba > maxBlocks || file.blocks > maxBlocks - file.startLba ||
        file.blocks != fileBlocks(file.size) || row.number(11) != 0 || row.number(12) != 0 || row.number(13) != 1 ||
        std::find(states.begin(), states.end(), file.status) == states.end() ||
        (file.status == "COMPLETED" ? !record::identity(file.sha256) : !file.sha256.empty())) {
        throw std::runtime_error("Malformed file or extent metadata");
    }
    return file;
}

}  // namespace

FileRepository::FileRepository(const std::filesystem::path& root)
    : leases_(root / config::defaultMetadataDirectory / "file_leases"),
      db_(leases_.parent_path() / "storage_catalog.db"),
      identity_(initializeSchema(db_)) {
    recover();
}

std::shared_ptr<void> FileRepository::tryLease(std::uint64_t id) {
    return disk_file::lockCatalog(leases_ / (std::to_string(id) + ".lock"), false);
}

FileOperation FileRepository::insertOperation(std::uint64_t file, std::string_view type) {
    Statement insert(db_, "INSERT INTO file_operations(file_id,operation_type,state) VALUES(?1,?2,'RUNNING')");
    insert.bind(1, file);
    insert.bind(2, type);
    insert.run();
    const auto id = db_.insertedId();
    auto lease = tryLease(id);
    if (!lease) {
        throw std::runtime_error("File operation lease is already held");
    }
    return {id, file, std::move(lease)};
}

ImportReservation FileRepository::reserve(const LunInfo& lun, std::string_view name, std::uint64_t bytes) {
    Transaction transaction(db_);
    std::uint64_t startLba = 0;
    {
        Statement highWater(db_, R"sql(
SELECT coalesce(max(e.lun_start_lba + e.block_count),0), count(*), count(e.file_id)
FROM files f LEFT JOIN file_extents e ON e.file_id=f.file_id WHERE f.lun_id=?1
)sql");
        highWater.bind(1, lun.id);
        if (!highWater.step() || highWater.number(1) != highWater.number(2)) {
            throw std::runtime_error("Incomplete file extent metadata for " + lun.id);
        }
        startLba = highWater.number(0);
    }
    const auto blocks = fileBlocks(bytes);
    if (startLba > lun.blocks || blocks > lun.blocks - startLba) {
        throw std::runtime_error("Insufficient logical LUN space for file import");
    }
    Statement file(db_, R"sql(
INSERT INTO files(lun_id,original_name,file_size_bytes,import_status) VALUES(?1,?2,?3,'PENDING')
)sql");
    file.bind(1, lun.id);
    file.bind(2, name);
    file.bind(3, bytes);
    file.run();
    const auto id = db_.insertedId();

    Statement extent(db_, "INSERT INTO file_extents(file_id,lun_start_lba,block_count) VALUES(?1,?2,?3)");
    extent.bind(1, id);
    extent.bind(2, startLba);
    extent.bind(3, blocks);
    extent.run();

    auto operation = insertOperation(id, "IMPORT");
    transaction.commit();
    return {std::move(operation), startLba};
}

FileRecord FileRepository::find(std::uint64_t id) {
    Statement query(db_, std::string(fileColumns) + "WHERE f.file_id=?1");
    query.bind(1, id);
    if (!query.step()) {
        throw std::invalid_argument("File does not exist: file" + std::to_string(id));
    }
    return readFile(query);
}

std::vector<FileRecord> FileRepository::list(std::string_view lun) {
    Statement query(db_, std::string(fileColumns) + (lun.empty() ? "" : "WHERE f.lun_id=?1 ") + "ORDER BY f.file_id");
    if (!lun.empty()) {
        query.bind(1, lun);
    }
    std::vector<FileRecord> files;
    while (query.step()) {
        files.push_back(readFile(query));
    }
    return files;
}

FileOperation FileRepository::start(std::uint64_t file, std::string_view type) {
    Transaction transaction(db_);
    auto operation = insertOperation(file, type);
    transaction.commit();
    return operation;
}

void FileRepository::transitionImport(std::uint64_t file, std::string_view before, std::string_view after) {
    Statement update(db_, "UPDATE files SET import_status=?3,updated_at=CURRENT_TIMESTAMP "
                          "WHERE file_id=?1 AND import_status=?2");
    update.bind(1, file);
    update.bind(2, before);
    update.bind(3, after);
    update.run();
    db_.requireOneChange();
}

void FileRepository::finishImport(const FileOperation& operation, std::string_view hash) {
    Transaction transaction(db_);
    Statement update(db_, "UPDATE files SET import_status='COMPLETED',sha256=?2,updated_at=CURRENT_TIMESTAMP "
                          "WHERE file_id=?1 AND import_status='VERIFYING'");
    update.bind(1, operation.file);
    update.bind(2, hash);
    update.run();
    db_.requireOneChange();
    finishOperation(operation.id, "COMPLETED", {}, hash);
    transaction.commit();
}

void FileRepository::markImportFailed(std::uint64_t file) {
    Statement update(db_, "UPDATE files SET import_status='FAILED',updated_at=CURRENT_TIMESTAMP "
                          "WHERE file_id=?1 AND import_status IN ('PENDING','WRITING','VERIFYING')");
    update.bind(1, file);
    update.run();
    db_.requireOneChange();
}

void FileRepository::failImport(const FileOperation& operation, std::string_view error) {
    Transaction transaction(db_);
    markImportFailed(operation.file);
    finishOperation(operation.id, "FAILED", error, {});
    transaction.commit();
}

void FileRepository::finishOperation(std::uint64_t id, std::string_view state, std::string_view error,
                                     std::string_view hash) {
    Statement update(db_, "UPDATE file_operations SET state=?2,error_message=?3,actual_sha256=nullif(?4,''),"
                          "completed_at=CURRENT_TIMESTAMP WHERE operation_id=?1 AND state='RUNNING'");
    update.bind(1, id);
    update.bind(2, state);
    update.bind(3, error.substr(0, 2048));
    update.bind(4, hash);
    update.run();
    db_.requireOneChange();
}

void FileRepository::finish(const FileOperation& operation, std::string_view state, std::string_view error,
                            std::string_view hash) {
    finishOperation(operation.id, state, error, hash);
}

void FileRepository::recover() {
    {
        Statement sample(db_, "SELECT 1 FROM file_operations WHERE state='RUNNING' LIMIT 1");
        if (!sample.step()) {
            return;
        }
    }
    Transaction transaction(db_);
    struct Interrupted {
        std::uint64_t operation;
        std::uint64_t file;
        bool importing;
        std::shared_ptr<void> lease;
    };
    std::vector<Interrupted> interrupted;
    {
        Statement rows(db_, "SELECT operation_id,file_id,operation_type FROM file_operations WHERE state='RUNNING'");
        while (rows.step()) {
            const auto id = rows.number(0);
            if (auto lease = tryLease(id)) {
                interrupted.push_back({id, rows.number(1), rows.text(2) == "IMPORT", std::move(lease)});
            }
        }
    }
    for (const auto& entry : interrupted) {
        if (entry.importing) {
            markImportFailed(entry.file);
        }
        finishOperation(entry.operation, "INTERRUPTED", "Worker process exited before completion", {});
    }
    transaction.commit();
}

}  // namespace raid::metadata
