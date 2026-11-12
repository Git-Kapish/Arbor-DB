#include "concurrency/transaction_manager.h"

#include "recovery/recovery_manager.h"

namespace arbordb {

TransactionManager::TransactionManager(LogManager* log, BufferPoolManager* bpm, ConcurrencyControlManager* cc,
                                       txn_id_t first_txn_id)
    : log_(log), bpm_(bpm), cc_(cc), next_id_(first_txn_id) {}

std::unique_ptr<Transaction> TransactionManager::Begin() {
    std::lock_guard<std::mutex> g(mu_);
    auto txn = std::make_unique<Transaction>(next_id_++);
    LogRecord r;
    r.type = LogType::kBegin;
    r.txn_id = txn->Id();
    txn->last_lsn = log_->Append(&r);
    active_[txn->Id()] = txn.get();
    if (cc_) cc_->OnBegin(txn.get());  // MVCC: choose the snapshot
    return txn;
}

void TransactionManager::Commit(Transaction* txn) {
    LogRecord r;
    r.type = LogType::kCommit;
    r.txn_id = txn->Id();
    r.prev_lsn = txn->last_lsn;
    txn->last_lsn = log_->Append(&r);
    log_->FlushTo(txn->last_lsn);  // commit point: durable before we report success
    txn->SetState(TxnState::kCommitted);
    txn->UndoHooks().clear();
    if (cc_) cc_->OnCommit(txn);  // MVCC: stamp this transaction's versions with its commit timestamp
    if (cc_) cc_->ReleaseAllLocks(txn);  // strict 2PL: release only after the outcome is durable
    std::lock_guard<std::mutex> g(mu_);
    active_.erase(txn->Id());
}

void TransactionManager::Abort(Transaction* txn) {
    LogRecord r;
    r.type = LogType::kAbort;
    r.txn_id = txn->Id();
    r.prev_lsn = txn->last_lsn;
    txn->last_lsn = log_->Append(&r);
    lsn_t chain = txn->last_lsn;
    RecoveryManager::UndoChain(log_, bpm_, txn->Id(), txn->last_lsn, &chain);
    LogRecord end;
    end.type = LogType::kEnd;
    end.txn_id = txn->Id();
    end.prev_lsn = chain;
    txn->last_lsn = log_->Append(&end);
    auto& hooks = txn->UndoHooks();
    for (auto it = hooks.rbegin(); it != hooks.rend(); ++it) (*it)();
    hooks.clear();
    txn->SetState(TxnState::kAborted);
    if (cc_) cc_->OnAbort(txn);  // MVCC: drop this transaction's versions (heap changes are already undone)
    if (cc_) cc_->ReleaseAllLocks(txn);
    std::lock_guard<std::mutex> g(mu_);
    active_.erase(txn->Id());
}

void TransactionManager::Checkpoint() {
    // Sequence matters: record where redo must start, flush, THEN snapshot the active transactions.
    // Anything logged from redo_start onward is re-scanned by analysis, so concurrent activity is safe.
    LogRecord ck;
    ck.type = LogType::kCheckpoint;
    ck.redo_start_lsn = log_->LastLsn() + 1;
    bpm_->FlushAll();
    bpm_->Disk()->Sync();  // fsync: the checkpoint promises these pages survive power loss, not just a process kill
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto& [id, t] : active_) ck.active_txns.push_back({id, t->last_lsn.load()});
    }
    lsn_t lsn = log_->Append(&ck);
    log_->FlushTo(lsn);
    log_->SetCheckpointLsn(lsn);
}

std::size_t TransactionManager::ActiveCount() {
    std::lock_guard<std::mutex> g(mu_);
    return active_.size();
}

}  // namespace arbordb
