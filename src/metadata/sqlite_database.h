#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace raid::metadata {

class Database {
public:
    static constexpr int busyTimeoutMs = 2000;
    explicit Database(const std::filesystem::path& path);
    void execute(const char* sql);
    void check(int result, std::string_view action) const;
    sqlite3* handle() const noexcept { return db_.get(); }
    std::uint64_t insertedId() const;
    void requireOneChange() const;

private:
    std::shared_ptr<void> protectedFile_;
    std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> db_{nullptr, sqlite3_close_v2};
};

class Statement {
public:
    Statement(Database& db, std::string_view sql);
    void bind(int index, std::string_view text);
    void bind(int index, std::uint64_t value);
    bool step();
    void run();
    std::uint64_t number(int column) const;
    std::string text(int column) const;

private:
    Database& db_;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt_{nullptr, sqlite3_finalize};
};

class Transaction {
public:
    explicit Transaction(Database& db);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit();

private:
    Database& db_;
    bool active_ = true;
};

}  // namespace raid::metadata
