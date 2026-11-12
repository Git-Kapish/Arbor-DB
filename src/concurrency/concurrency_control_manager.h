#pragma once

#include <string>

#include "concurrency/transaction.h"

namespace arbordb {

enum class LockMode { kShared, kExclusive };
enum class WriteKind { kInsert, kUpdate, kDelete };

// Strategy pattern: the data layer talks to concurrency control only through this interface.
// TwoPLManager (strict 2PL) and MVCCManager (snapshot isolation) are the two strategies. The lock
// methods are the original contract; the remaining virtuals are hooks with 2PL-neutral defaults, so a
// scheme that needs no versioning (2PL) ignores them.
class ConcurrencyControlManager {
public:
    virtual ~ConcurrencyControlManager() = default;

    // Blocks until granted. Returns false if the transaction must abort; the reason is left in
    // txn->abort_reason (deadlock victim by default, "write-write conflict" for MVCC).
    virtual bool AcquireLock(Transaction* txn, const RID& rid, LockMode mode) = 0;
    virtual void ReleaseLock(Transaction* txn, const RID& rid) = 0;
    // Called at commit/abort: strict 2PL releases everything here and nowhere else.
    virtual void ReleaseAllLocks(Transaction* txn) = 0;

    // Do reads take S locks (2PL), or are they lock-free snapshot reads (MVCC)?
    virtual bool TakesReadLocks() const { return true; }

    // Lifecycle hooks. OnBegin runs when the transaction starts (MVCC: pick its snapshot); OnCommit runs
    // after the COMMIT record is durable; OnAbort runs after the heap changes were rolled back. Both are
    // followed by ReleaseAllLocks.
    virtual void OnBegin(Transaction*) {}
    virtual void OnCommit(Transaction*) {}
    virtual void OnAbort(Transaction*) {}

    // Called by TableHeap, under the page latch and BEFORE the change is applied, with the tuple as it
    // was (before_exists is false for inserts).
    virtual void OnWrite(Transaction*, const RID&, WriteKind, bool /*before_exists*/, const std::string& /*before*/) {}

    // Called by TableHeap on every read, under the page latch, with the newest version stored in the heap.
    // Returns whether the transaction may see the tuple, and the bytes it should see in *out.
    // Default: the newest version is the only version.
    virtual bool ResolveRead(Transaction*, const RID&, bool latest_exists, const std::string& latest, std::string* out) {
        if (latest_exists) *out = latest;
        return latest_exists;
    }
};

}  // namespace arbordb
