#include "db/database.h"

#include <cstdio>
#include <filesystem>
#include <functional>

#include "query/binder/binder.h"
#include "query/executor/executor.h"
#include "query/optimizer/optimizer.h"
#include "query/parser/parser.h"

namespace arbordb {

std::vector<std::string> QueryResult::RowStrings() const {
    std::vector<std::string> out;
    for (auto& r : rows) {
        std::string s;
        for (std::size_t i = 0; i < r.size(); i++) s += (i ? "|" : "") + ValueToString(r[i]);
        out.push_back(s);
    }
    return out;
}

Database::Database(const std::string& dir, DatabaseOptions opts) : opts_(opts) {
    std::filesystem::create_directories(dir);
    dm_ = std::make_unique<DiskManager>(dir + "/data.db");
    log_ = std::make_unique<LogManager>(dir + "/wal.log");
    bpm_ = std::make_unique<BufferPoolManager>(opts.buffer_pool_pages, dm_.get(), log_.get());
    recovery_ = RecoveryManager(log_.get(), bpm_.get()).Recover();  // ARIES: analysis, redo, undo

    std::filesystem::remove(dir + "/index.db");  // index pages are never trusted across restarts
    index_dm_ = std::make_unique<DiskManager>(dir + "/index.db");
    index_bpm_ = std::make_unique<BufferPoolManager>(opts.index_pool_pages, index_dm_.get());

    if (opts.concurrency == ConcurrencyMode::kMVCC) {
        auto m = std::make_unique<MVCCManager>();
        locks_ = &m->Locks();
        cc_ = std::move(m);
    } else {
        auto m = std::make_unique<TwoPLManager>();
        locks_ = &m->Locks();
        cc_ = std::move(m);
    }

    tm_ = std::make_unique<TransactionManager>(log_.get(), bpm_.get(), cc_.get(), recovery_.max_txn_id + 1);
    catalog_ = std::make_unique<Catalog>(dir + "/catalog.meta", bpm_.get(), index_bpm_.get(), log_.get(), cc_.get());
    catalog_->Load();
    catalog_->RebuildIndexes();
    tm_->Checkpoint();  // recovered state is now on disk; the next restart starts from here
}

Database::~Database() {
    if (crashed_) return;  // behave like kill -9: no flush, no checkpoint
    try {
        tm_->Checkpoint();
        log_->Flush();
    } catch (...) {
    }
}

Session::~Session() {
    try {
        RollbackOpenTxn();
    } catch (...) {
    }
}

void Session::RollbackOpenTxn() {
    if (!txn_) return;
    db_->tm_->Abort(txn_.get());
    txn_.reset();
}

std::vector<QueryResult> Session::ExecuteScript(const std::string& sql) {
    std::vector<QueryResult> out;
    for (auto& st : ParseSql(sql)) out.push_back(ExecuteStatement(st));
    return out;
}

QueryResult Session::Execute(const std::string& sql) {
    auto stmts = ParseSql(sql);
    if (stmts.size() != 1) throw ParseError("expected exactly one statement, got " + std::to_string(stmts.size()));
    return ExecuteStatement(stmts[0]);
}

// Plan building (parse/bind/optimize) has no side effects, so it happens before any transaction starts.
QueryResult Session::RunPlanned(const std::function<PlanPtr()>& build, bool is_query) {
    std::shared_lock<std::shared_mutex> ddl(db_->ddl_mutex_);
    PlanPtr plan = build();
    bool autocommit = !txn_;
    std::unique_ptr<Transaction> local;
    Transaction* txn;
    if (autocommit) {
        local = db_->tm_->Begin();
        txn = local.get();
    } else {
        txn = txn_.get();
    }
    QueryResult res;
    res.is_query = is_query;
    try {
        ExecContext ctx{txn};
        auto ex = ExecutorFactory::Create(*plan, &ctx);
        ex->Init();
        for (auto& c : ex->OutputSchema().Columns()) res.columns.push_back(c.name);
        while (auto t = ex->Next()) res.rows.push_back(std::move(t->values));
        if (autocommit) db_->tm_->Commit(txn);
    } catch (const TransactionAbortedException&) {
        std::string reason = txn->abort_reason;
        db_->tm_->Abort(txn);
        if (!autocommit) txn_.reset();
        throw DbError(std::string("transaction aborted (") + reason + "); it was rolled back, retry it");
    } catch (...) {
        db_->tm_->Abort(txn);  // ExecutionError etc.: undo everything this transaction did
        if (!autocommit) txn_.reset();
        throw;
    }
    if (!is_query) {
        res.rows_affected = res.rows.empty() ? 0 : std::get<int64_t>(res.rows[0][0]);
        res.rows.clear();
        res.columns.clear();
        res.message = std::to_string(res.rows_affected) + (res.rows_affected == 1 ? " row affected" : " rows affected");
    }
    return res;
}

QueryResult Session::Ddl(const Statement& st) {
    std::unique_lock<std::shared_mutex> ddl(db_->ddl_mutex_);
    Catalog& cat = *db_->catalog_;
    QueryResult r;
    if (auto* ct = std::get_if<CreateTableStmt>(&st)) {
        std::vector<Column> cols;
        for (auto& c : ct->columns) {
            if (c.primary_key && c.type != TypeId::kInteger) throw BindError("PRIMARY KEY is supported on INTEGER columns only");
            cols.push_back(Column{c.name, c.type, ""});
        }
        cat.CreateTable(ct->name, Schema(cols));
        for (auto& c : ct->columns)
            if (c.primary_key) cat.CreateIndex(Catalog::Lower(ct->name) + "_" + c.name + "_pkey", ct->name, c.name);
        r.message = "CREATE TABLE";
    } else if (auto* ci = std::get_if<CreateIndexStmt>(&st)) {
        cat.CreateIndex(ci->name, ci->table, ci->column);
        r.message = "CREATE INDEX";
    } else if (auto* dt = std::get_if<DropTableStmt>(&st)) {
        cat.DropTable(dt->name);
        r.message = "DROP TABLE";
    }
    return r;
}

QueryResult Session::ExecuteStatement(const Statement& st) {
    if (std::holds_alternative<CreateTableStmt>(st) || std::holds_alternative<CreateIndexStmt>(st) ||
        std::holds_alternative<DropTableStmt>(st))
        return Ddl(st);

    if (auto* t = std::get_if<TxnStmt>(&st)) {
        QueryResult r;
        switch (t->kind) {
            case TxnStmt::kBegin:
                if (txn_) throw ExecutionError("already in a transaction");
                txn_ = db_->tm_->Begin();
                r.message = "BEGIN";
                break;
            case TxnStmt::kCommit:
                if (!txn_) throw ExecutionError("no transaction in progress");
                db_->tm_->Commit(txn_.get());
                txn_.reset();
                r.message = "COMMIT";
                break;
            case TxnStmt::kRollback:
                if (!txn_) throw ExecutionError("no transaction in progress");
                RollbackOpenTxn();
                r.message = "ROLLBACK";
                break;
        }
        return r;
    }
    if (std::holds_alternative<CheckpointStmt>(st)) {
        db_->tm_->Checkpoint();
        QueryResult r;
        r.message = "CHECKPOINT";
        return r;
    }

    Catalog* cat = db_->catalog_.get();
    if (auto* s = std::get_if<SelectStmt>(&st))
        return RunPlanned([&] { return Optimizer(db_->opts_.concurrency == ConcurrencyMode::kTwoPL).Optimize(Binder(cat).BindSelect(*s)); }, true);
    if (auto* e = std::get_if<ExplainStmt>(&st)) {
        std::shared_lock<std::shared_mutex> ddl(db_->ddl_mutex_);
        PlanPtr plan = Optimizer(db_->opts_.concurrency == ConcurrencyMode::kTwoPL).Optimize(Binder(cat).BindSelect(*e->select));
        QueryResult r;
        r.is_query = true;
        r.columns = {"plan"};
        std::string text = PlanToString(*plan);
        std::size_t start = 0;
        while (start < text.size()) {
            std::size_t nl = text.find('\n', start);
            r.rows.push_back({Value(text.substr(start, nl - start))});
            start = nl + 1;
        }
        return r;
    }
    if (auto* s = std::get_if<InsertStmt>(&st))
        return RunPlanned([&] { return Optimizer(db_->opts_.concurrency == ConcurrencyMode::kTwoPL).Optimize(Binder(cat).BindInsert(*s)); }, false);
    if (auto* s = std::get_if<UpdateStmt>(&st))
        return RunPlanned([&] { return Optimizer(db_->opts_.concurrency == ConcurrencyMode::kTwoPL).Optimize(Binder(cat).BindUpdate(*s)); }, false);
    if (auto* s = std::get_if<DeleteStmt>(&st))
        return RunPlanned([&] { return Optimizer(db_->opts_.concurrency == ConcurrencyMode::kTwoPL).Optimize(Binder(cat).BindDelete(*s)); }, false);
    throw ExecutionError("unsupported statement");
}

}  // namespace arbordb
