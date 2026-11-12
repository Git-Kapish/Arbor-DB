#pragma once

#include <map>
#include <memory>
#include <mutex>

#include "concurrency/concurrency_control_manager.h"
#include "recovery/log_manager.h"

namespace arbordb {

class TransactionManager {
public:
    // cc may be null (no locking, e.g. in recovery tests). first_txn_id: continue after recovery's max id.
    TransactionManager(LogManager* log, BufferPoolManager* bpm, ConcurrencyControlManager* cc,
                       txn_id_t first_txn_id = 1);

    // Returned pointer stays valid until the transaction is committed/aborted and released by the caller.
    std::unique_ptr<Transaction> Begin();
    // Durable on return (log flushed through the COMMIT record); then locks are released.
    void Commit(Transaction* txn);
    // Undoes the transaction's changes (writing CLRs), runs index undo hooks, then releases locks.
    void Abort(Transaction* txn);

    // Sharp checkpoint: flush all dirty pages (WAL rule applies), then log CHECKPOINT with the active
    // transaction table and record it as the master checkpoint.
    void Checkpoint();

    std::size_t ActiveCount();

private:
    LogManager* log_;
    BufferPoolManager* bpm_;
    ConcurrencyControlManager* cc_;
    std::mutex mu_;
    txn_id_t next_id_;
    std::map<txn_id_t, Transaction*> active_;
};

}  // namespace arbordb
