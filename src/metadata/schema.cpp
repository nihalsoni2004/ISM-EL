#include "metadata/schema.h"

#include "recovery/record.h"

#include <stdexcept>

namespace raid::metadata {
namespace {

constexpr std::uint64_t applicationId = 1396983353;
constexpr std::uint64_t schemaVersion = 1;

constexpr auto initialSchema = R"sql(
CREATE TABLE schema_version (
    version INTEGER PRIMARY KEY CHECK(version > 0),
    applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
) STRICT;
CREATE TABLE catalog_identity (
    singleton INTEGER PRIMARY KEY CHECK(singleton = 1),
    catalog_id TEXT NOT NULL CHECK(length(catalog_id) = 64 AND catalog_id NOT GLOB '*[^0-9a-f]*')
) STRICT;
CREATE TABLE files (
    file_id INTEGER PRIMARY KEY AUTOINCREMENT,
    lun_id TEXT NOT NULL CHECK(length(lun_id) BETWEEN 4 AND 23),
    original_name TEXT NOT NULL CHECK(length(original_name) BETWEEN 1 AND 255),
    file_size_bytes INTEGER NOT NULL CHECK(file_size_bytes >= 0),
    sha256 TEXT CHECK(length(sha256) = 64 AND sha256 NOT GLOB '*[^0-9a-f]*'),
    import_status TEXT NOT NULL CHECK(import_status IN ('PENDING','WRITING','VERIFYING','COMPLETED','FAILED')),
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    format_version INTEGER NOT NULL DEFAULT 1 CHECK(format_version = 1),
    CHECK((import_status = 'COMPLETED') = (sha256 IS NOT NULL))
) STRICT;
CREATE INDEX files_by_lun ON files(lun_id, file_id);
CREATE TABLE file_extents (
    extent_id INTEGER PRIMARY KEY,
    file_id INTEGER NOT NULL UNIQUE REFERENCES files(file_id),
    file_offset_bytes INTEGER NOT NULL DEFAULT 0 CHECK(file_offset_bytes = 0),
    lun_start_lba INTEGER NOT NULL CHECK(lun_start_lba BETWEEN 0 AND 4503599627370495),
    block_count INTEGER NOT NULL CHECK(block_count BETWEEN 0 AND 4503599627370495 - lun_start_lba),
    extent_order INTEGER NOT NULL DEFAULT 0 CHECK(extent_order = 0)
) STRICT;
CREATE TABLE file_operations (
    operation_id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_id INTEGER NOT NULL REFERENCES files(file_id),
    operation_type TEXT NOT NULL CHECK(operation_type IN ('IMPORT','EXPORT','VERIFY')),
    state TEXT NOT NULL CHECK(state IN ('RUNNING','COMPLETED','FAILED','INTERRUPTED')),
    started_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    completed_at TEXT,
    error_message TEXT NOT NULL DEFAULT '',
    actual_sha256 TEXT CHECK(length(actual_sha256) = 64 AND actual_sha256 NOT GLOB '*[^0-9a-f]*'),
    CHECK((state = 'RUNNING') = (completed_at IS NULL))
) STRICT;
CREATE UNIQUE INDEX one_import ON file_operations(file_id) WHERE operation_type = 'IMPORT';
CREATE INDEX operations_by_file ON file_operations(file_id, operation_id);
CREATE INDEX running_operations ON file_operations(operation_id) WHERE state = 'RUNNING';
CREATE TRIGGER extent_validate BEFORE INSERT ON file_extents BEGIN
    SELECT CASE WHEN NEW.block_count != (SELECT file_size_bytes / 4096 + (file_size_bytes % 4096 != 0)
                      FROM files WHERE file_id = NEW.file_id)
        THEN RAISE(ABORT, 'Extent does not match file byte size') END;
    SELECT CASE WHEN NEW.block_count > 0 AND EXISTS (
        SELECT 1 FROM files AS owner JOIN files AS other ON other.lun_id = owner.lun_id
        JOIN file_extents AS extent ON extent.file_id = other.file_id
        WHERE owner.file_id = NEW.file_id AND extent.block_count > 0
            AND extent.lun_start_lba < NEW.lun_start_lba + NEW.block_count
            AND NEW.lun_start_lba < extent.lun_start_lba + extent.block_count
    ) THEN RAISE(ABORT, 'Overlapping LUN file extent') END;
END;
CREATE TRIGGER extent_immutable BEFORE UPDATE ON file_extents BEGIN
    SELECT RAISE(ABORT, 'File extent is immutable');
END;
CREATE TRIGGER placement_immutable BEFORE UPDATE OF lun_id, file_size_bytes ON files BEGIN
    SELECT RAISE(ABORT, 'File placement is immutable');
END;
INSERT INTO schema_version(version) VALUES(1);
INSERT INTO catalog_identity(singleton, catalog_id) VALUES(1, lower(hex(randomblob(32))));
PRAGMA application_id=1396983353;
PRAGMA user_version=1;
)sql";

std::uint64_t scalar(Database& db, std::string_view sql) {
    Statement row(db, sql);
    if (!row.step()) {
        throw std::runtime_error("SQLite schema query returned no result");
    }
    return row.number(0);
}

void checkVersion(Database& db) {
    if (scalar(db, "PRAGMA user_version") != schemaVersion || scalar(db, "PRAGMA application_id") != applicationId ||
        scalar(db, "SELECT count(*) FROM schema_version WHERE version = 1") != 1 ||
        scalar(db, "SELECT count(*) FROM schema_version") != 1) {
        throw std::runtime_error("Unsupported SQLite file catalog schema or application ID");
    }
}

}  // namespace

std::string initializeSchema(Database& db) {
    {
        Statement mode(db, "PRAGMA journal_mode");
        if (!mode.step() || mode.text(0) != "delete") {
            throw std::runtime_error("SQLite file catalog requires DELETE rollback-journal mode");
        }
    }
    if (scalar(db, "PRAGMA user_version") == 0) {
        Transaction migration(db);
        if (scalar(db, "PRAGMA user_version") == 0) {
            if (scalar(db, "PRAGMA application_id") != 0 ||
                scalar(db, "SELECT count(*) FROM sqlite_schema") != 0) {
                throw std::runtime_error("Refusing to initialize an unrecognized SQLite database");
            }
            db.execute(initialSchema);
        }
        checkVersion(db);
        migration.commit();
    } else {
        checkVersion(db);
    }

    if (scalar(db, "PRAGMA foreign_keys") != 1 || scalar(db, "PRAGMA synchronous") != 2) {
        throw std::runtime_error("SQLite durability or foreign key settings were not applied");
    }
    Statement identity(db, "SELECT catalog_id FROM catalog_identity WHERE singleton=1");
    if (!identity.step()) {
        throw std::runtime_error("SQLite file catalog identity is missing");
    }
    auto result = identity.text(0);
    if (!record::identity(result) || identity.step()) {
        throw std::runtime_error("SQLite file catalog identity is invalid");
    }
    return result;
}

}  // namespace raid::metadata
