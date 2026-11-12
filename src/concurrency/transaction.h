#pragma once

#include <atomic>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/config.h"

namespace arbordb {

enum class TxnState { kActive, kCommitted, kAborted };

class Transaction {
public:
    explicit Transaction(txn_id_t id) : id_(id) {}

    txn_id_t Id() const { return id_; }
    TxnState State() const { return state_.load(); }
    void SetState(TxnState s) { state_.store(s); }

    // LSN of this transaction's most recent log record (head of its prev_lsn chain).
    std::atomic<lsn_t> last_lsn{kInvalidLsn};

    // In-memory compensation (e.g. undoing B+Tree changes). Run in reverse order on abort,
    // before locks are released. Index pages are not WAL-logged, so this is how index changes roll back.
    void AddUndoHook(std::function<void()> h) { undo_hooks_.push_back(std::move(h)); }
    std::vector<std::function<void()>>& UndoHooks() { return undo_hooks_; }

private:
    txn_id_t id_;
    std::atomic<TxnState> state_{TxnState::kActive};
    std::vector<std::function<void()>> undo_hooks_;
};

// Thrown by data-access code when a lock cannot be granted because the transaction was chosen as a
// deadlock victim. The caller must roll the transaction back.
class TransactionAbortedException : public std::runtime_error {
public:
    explicit TransactionAbortedException(txn_id_t id)
        : std::runtime_error("transaction " + std::to_string(id) + " aborted (deadlock victim)"), txn_id(id) {}
    txn_id_t txn_id;
};

}  // namespace arbordb
