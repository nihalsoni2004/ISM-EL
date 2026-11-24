#include "metadata/sqlite_database.h"

#include "disk/image_file.h"

#include <limits>
#include <stdexcept>

namespace raid::metadata {

Database::Database(const std::filesystem::path& path) : protectedFile_(disk_file::pinDatabase(path)) {
    sqlite3* connection = nullptr;
    const auto result = sqlite3_open_v2(path.u8string().c_str(), &connection,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW | SQLITE_OPEN_EXRESCODE, nullptr);
    db_.reset(connection);
    check(result, "Open SQLite file catalog");
    if (sqlite3_db_readonly(handle(), "main") != 0) {
        throw std::runtime_error("SQLite file catalog must be writable");
    }
    check(sqlite3_busy_timeout(handle(), busyTimeoutMs), "Set SQLite busy timeout");
    execute("PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL; PRAGMA trusted_schema=OFF;");
}

void Database::check(int result, std::string_view action) const {
    if (result == SQLITE_OK || result == SQLITE_ROW || result == SQLITE_DONE) {
        return;
    }
    const auto primary = result & 0xff;
    const auto reason = primary == SQLITE_BUSY || primary == SQLITE_LOCKED ? "SQLite catalog busy or locked"
        : primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB ? "SQLite catalog is corrupt or invalid"
        : "SQLite error";
    throw std::runtime_error(std::string(action) + ": " + reason + " (" + std::to_string(result) + "): " +
                             (handle() ? sqlite3_errmsg(handle()) : sqlite3_errstr(result)));
}

void Database::execute(const char* sql) {
    check(sqlite3_exec(handle(), sql, nullptr, nullptr, nullptr), "Execute SQLite statement");
}

std::uint64_t Database::insertedId() const {
    const auto value = sqlite3_last_insert_rowid(handle());
    if (value <= 0) {
        throw std::runtime_error("SQLite did not generate a valid identifier");
    }
    return static_cast<std::uint64_t>(value);
}

void Database::requireOneChange() const {
    if (sqlite3_changes(handle()) != 1) {
        throw std::runtime_error("Unexpected file catalog state transition");
    }
}

Statement::Statement(Database& db, std::string_view sql) : db_(db) {
    sqlite3_stmt* statement = nullptr;
    const auto result = sqlite3_prepare_v2(db_.handle(), sql.data(), static_cast<int>(sql.size()), &statement, nullptr);
    stmt_.reset(statement);
    db_.check(result, "Prepare SQLite statement");
}

void Statement::bind(int index, std::string_view text) {
    db_.check(sqlite3_bind_text64(stmt_.get(), index, text.empty() ? "" : text.data(), text.size(),
                                 SQLITE_TRANSIENT, SQLITE_UTF8), "Bind SQLite text");
}

void Statement::bind(int index, std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
        throw std::invalid_argument("Value exceeds the SQLite integer range");
    }
    db_.check(sqlite3_bind_int64(stmt_.get(), index, static_cast<sqlite3_int64>(value)), "Bind SQLite integer");
}

bool Statement::step() {
    const auto result = sqlite3_step(stmt_.get());
    db_.check(result, "Step SQLite statement");
    return result == SQLITE_ROW;
}

void Statement::run() {
    if (step()) {
        throw std::runtime_error("Unexpected row for SQLite write");
    }
}

std::uint64_t Statement::number(int column) const {
    if (sqlite3_column_type(stmt_.get(), column) != SQLITE_INTEGER || sqlite3_column_int64(stmt_.get(), column) < 0) {
        throw std::runtime_error("Invalid integer in file catalog");
    }
    return static_cast<std::uint64_t>(sqlite3_column_int64(stmt_.get(), column));
}

std::string Statement::text(int column) const {
    if (sqlite3_column_type(stmt_.get(), column) != SQLITE_TEXT) {
        throw std::runtime_error("Invalid text in file catalog");
    }
    const auto bytes = sqlite3_column_text(stmt_.get(), column);
    if (!bytes) {
        throw std::runtime_error("Could not read file catalog text");
    }
    return {reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(sqlite3_column_bytes(stmt_.get(), column))};
}

Transaction::Transaction(Database& db) : db_(db) {
    db_.execute("BEGIN IMMEDIATE");
}

Transaction::~Transaction() {
    if (active_) {
        sqlite3_exec(db_.handle(), "ROLLBACK", nullptr, nullptr, nullptr);
    }
}

void Transaction::commit() {
    db_.execute("COMMIT");
    active_ = false;
}

}  // namespace raid::metadata
