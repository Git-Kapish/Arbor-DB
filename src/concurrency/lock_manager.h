#pragma once

#include <condition_variable>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "concurrency/concurrency_control_manager.h"

namespace arbordb {

struct RIDHash {
    std::size_t operator()(const RID& r) const {
        return std::hash<uint64_t>()((static_cast<uint64_t>(static_cast<uint32_t>(r.page_id)) << 32) | r.slot);
    }
};

// Row-level lock table: shared/exclusive locks per RID, with S->X upgrades, FIFO queueing, and
// deadlock detection on a wait-for graph. This is ArborDB's own lock data structure -- not a mutex per
// row -- because 2PL needs to know who holds what and who waits for whom.
//
// Deadlock handling: every time a transaction is about to block (and again each time it wakes still
// blocked) the manager builds the wait-for graph from all lock queues and looks for cycles
// (DFS over transactions in ascending id order, so the result is deterministic). For each cycle the
// YOUNGEST transaction (largest id) is chosen as victim: it is flagged aborted and woken, and its
// Lock() call returns false. The victim's caller must roll back, which releases its locks.
// Alternative victim policies (fewest locks held / least work done) are a "what I'd do at scale" item.
// Detection is event-driven, not timeout-based, so a deadlock is resolved immediately and never hangs.
//
// Scope: row locks only. There are no predicate/range locks, so phantoms are possible (README Known Gaps).
class LockManager {
public:
    // Blocks until granted. false => the transaction was marked aborted (deadlock victim) and holds no
    // new lock from this call; the caller must abort it.
    bool Lock(Transaction* txn, const RID& rid, LockMode mode);
    void Unlock(Transaction* txn, const RID& rid);
    void UnlockAll(Transaction* txn);

    std::size_t DeadlocksDetected();
    std::size_t LockCount(txn_id_t txn);  // locks currently held (tests)

private:
    struct Request {
        Transaction* txn;
        LockMode mode;
        bool granted;
        bool upgrading;  // holds S, waiting to become X
    };
    struct Queue {
        std::list<Request> requests;  // arrival order
        std::condition_variable cv;
    };
    using Graph = std::map<txn_id_t, std::set<txn_id_t>>;

    static bool Conflicts(LockMode a, LockMode b) { return a == LockMode::kExclusive || b == LockMode::kExclusive; }
    static LockMode Effective(const Request& r) { return r.upgrading ? LockMode::kExclusive : r.mode; }
    bool CanGrant(Queue& q, std::list<Request>::iterator it);
    void BuildGraph(Graph* g);
    // Breaks every cycle by flagging victims; returns true if `self` was chosen.
    bool ResolveDeadlocks(txn_id_t self);
    void RemoveRequest(const RID& rid, Queue& q, std::list<Request>::iterator it, txn_id_t txn);

    std::mutex mu_;
    std::unordered_map<RID, std::shared_ptr<Queue>, RIDHash> table_;
    std::map<txn_id_t, std::set<RID>> held_;       // txn -> rows it holds locks on
    struct Waiter { Transaction* txn; Queue* queue; };
    std::map<txn_id_t, Waiter> waiting_;            // txn -> the queue it is blocked in
    std::size_t deadlocks_{0};
};

// The strict two-phase-locking strategy: locks accumulate as the transaction runs and are released
// only by ReleaseAllLocks(), which TransactionManager calls after COMMIT is durable or after rollback.
class TwoPLManager : public ConcurrencyControlManager {
public:
    bool AcquireLock(Transaction* txn, const RID& rid, LockMode mode) override { return lm_.Lock(txn, rid, mode); }
    void ReleaseLock(Transaction* txn, const RID& rid) override { lm_.Unlock(txn, rid); }
    void ReleaseAllLocks(Transaction* txn) override { lm_.UnlockAll(txn); }
    LockManager& Locks() { return lm_; }

private:
    LockManager lm_;
};

}  // namespace arbordb
