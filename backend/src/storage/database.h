#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "storage/config.h"
#include "util/status.h"
#include "util/status_or.h"

namespace mmcr::storage {

class Statement;
class Transaction;

class Database {
public:
    Database();
    ~Database();

    Database(Database&&) noexcept;
    auto operator=(Database&&) noexcept -> Database&;

    Database(const Database&) = delete;
    auto operator=(const Database&) -> Database& = delete;

    [[nodiscard]] util::Status Open(const SqliteConfig& config);
    void Close();

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] const SqliteConfig& config() const noexcept;
    [[nodiscard]] util::Status Execute(std::string_view sql) const;
    [[nodiscard]] util::StatusOr<Statement> Prepare(std::string_view sql) const;
    [[nodiscard]] util::StatusOr<Transaction> BeginTransaction();
    [[nodiscard]] util::StatusOr<std::int64_t> LastInsertRowId() const;

private:
    friend class Statement;
    friend class Transaction;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Statement {
public:
    enum class StepResult : std::uint8_t {
        kRow,
        kDone,
    };

    Statement();
    ~Statement();

    Statement(Statement&&) noexcept;
    auto operator=(Statement&&) noexcept -> Statement&;

    Statement(const Statement&) = delete;
    auto operator=(const Statement&) -> Statement& = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] util::Status BindInt64(int index, std::int64_t value);
    [[nodiscard]] util::Status BindText(int index, std::string_view value);
    [[nodiscard]] util::Status BindNull(int index);
    [[nodiscard]] util::StatusOr<StepResult> Step();
    [[nodiscard]] std::int64_t ColumnInt64(int index) const;
    [[nodiscard]] std::string ColumnText(int index) const;
    [[nodiscard]] util::Status Reset();

private:
    friend class Database;

    struct Impl;
    explicit Statement(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

class Transaction {
public:
    Transaction();
    ~Transaction();

    Transaction(Transaction&&) noexcept;
    auto operator=(Transaction&&) noexcept -> Transaction&;

    Transaction(const Transaction&) = delete;
    auto operator=(const Transaction&) -> Transaction& = delete;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] util::Status Commit();
    [[nodiscard]] util::Status Rollback();

private:
    friend class Database;

    struct Impl;
    explicit Transaction(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace mmcr::storage