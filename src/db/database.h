#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "concurrency/lock_manager.h"
#include "concurrency/transaction_manager.h"
#include "query/parser/ast.h"
#include "query/plan.h"
#include "recovery/recovery_manager.h"

namespace arbordb {

struct QueryResult {
    std::vector<std::string> columns;
    std::vector<std::vector<Value>> rows;
    std::string message;        // "CREATE TABLE", "3 rows affected", plan text for EXPLAIN, ...
    bool is_query{false};       // true when `rows` is a result set
    int64_t rows_affected{0};
    // Convenience for tests: each row rendered as "a|b|c".
    std::vector<std::string> RowStrings() const;
};

struct DatabaseOptions {
    std::size_t buffer_pool_pages = 512;
    std::size_t index_pool_pages = 256;
};

// The whole engine in one object: opening it runs ARIES recovery over the data/log files in `dir`,
// then loads the catalog and rebuilds the indexes. Destroying it shuts down cleanly (checkpoint);
// Crash() makes the destructor skip that, so dropping the object behaves like a process kill.
//
// Files in dir: data.db (heap pages), wal.log (+ .master), catalog.meta, index.db (scratch, rebuilt each start).
class Database {
public:
    explicit Database(const std::string& dir, DatabaseOptions opts = DatabaseOptions());
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void Crash() { crashed_ = true; }
    const RecoveryResult& LastRecovery() const { return recovery_; }
    Catalog& GetCatalog() { return *catalog_; }
    TransactionManager& Txns() { return *tm_; }
    LockManager& Locks() { return cc_.Locks(); }
    BufferPoolManager& Pool() { return *bpm_; }

private:
    friend class Session;
    DatabaseOptions opts_;
    bool crashed_{false};
    RecoveryResult recovery_;
    std::unique_ptr<DiskManager> dm_, index_dm_;
    std::unique_ptr<LogManager> log_;
    std::unique_ptr<BufferPoolManager> bpm_, index_bpm_;
    TwoPLManager cc_;
    std::unique_ptr<TransactionManager> tm_;
    std::unique_ptr<Catalog> catalog_;
    // DDL takes this exclusively, DML/queries share it: DDL never runs under a statement (documented gap:
    // an explicit transaction is not protected from DDL *between* its statements).
    std::shared_mutex ddl_mutex_;
};

// A client connection: owns at most one open transaction. Statements outside BEGIN..COMMIT run in
// their own (autocommit) transaction. Not thread-safe; use one Session per thread.
//
// Error policy: ParseError/BindError have no side effects and leave an open transaction alone.
// An ExecutionError or deadlock-victim abort rolls the whole transaction back (like PostgreSQL's
// "current transaction is aborted"), because statements are not atomic on their own.
class Session {
public:
    explicit Session(Database* db) : db_(db) {}
    ~Session();

    // All statements in the script, in order. Throws DbError on the first failing statement.
    std::vector<QueryResult> ExecuteScript(const std::string& sql);
    QueryResult Execute(const std::string& sql);  // exactly one statement
    bool InTransaction() const { return txn_ != nullptr; }
    // Test hook for crash simulation: forget the open transaction WITHOUT rolling it back.
    void Abandon() { txn_.reset(); }

private:
    QueryResult ExecuteStatement(const Statement& st);
    QueryResult RunPlanned(const std::function<PlanPtr()>& plan_builder, bool is_query);
    QueryResult Ddl(const Statement& st);
    void RollbackOpenTxn();

    Database* db_;
    std::unique_ptr<Transaction> txn_;
};

}  // namespace arbordb
