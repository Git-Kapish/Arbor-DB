#pragma once

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "concurrency/lock_manager.h"

namespace arbordb {

// SIMPLIFIED multi-version concurrency control providing SNAPSHOT ISOLATION.
//
// Isolation level actually provided (and what is NOT):
//   * no dirty reads, no non-repeatable reads, no lost updates, and no phantoms for sequential scans: every
//     transaction reads the database as of the commit timestamp current when it began, plus its own writes;
//   * readers take no locks and never block or get blocked;
//   * write-write conflicts follow first-committer-wins: writers take an exclusive row lock held to the end
//     of the transaction, and a writer that acquires the lock on a row some other transaction committed after
//     its snapshot is aborted ("write-write conflict");
//   * it is NOT serializable: write skew is possible (two transactions each read both of two rows and each
//     update a different one). tests/test_mvcc.cpp demonstrates that anomaly on purpose, next to strict 2PL
//     preventing it.
//
// Design (deliberately small):
//   * The heap always holds the NEWEST version of each tuple (committed, or uncommitted by its one writer),
//     with the same WAL/ARIES logging as under 2PL. Older committed versions live in an in-memory
//     VersionStore: per RID, a chain of {begin_ts, bytes}, newest first. Recovery needs none of it: after a
//     restart the heap holds only committed data, so the store starts empty (everything is "committed at ts 0").
//   * Commit timestamps come from one counter, assigned (and published) atomically under the store mutex
//     when a transaction that wrote something commits. A transaction's snapshot is the last published value.
//   * Garbage collection runs every kGcInterval commits (and on demand): versions that no active snapshot can
//     still need are dropped; chains whose newest version every snapshot can see are removed entirely.
//
// Known limits (also in README): B+Tree indexes are not versioned, so the optimizer does not use IndexScan
// in MVCC mode (indexes still enforce uniqueness); GC is a full sweep, not incremental; the version store is
// not bounded by memory pressure; only INSERT/UPDATE/DELETE of whole tuples are versioned (no DDL).
class MVCCManager : public ConcurrencyControlManager {
public:
    static constexpr std::size_t kGcInterval = 64;

    bool AcquireLock(Transaction* txn, const RID& rid, LockMode mode) override;
    void ReleaseLock(Transaction* txn, const RID& rid) override { locks_.Unlock(txn, rid); }
    void ReleaseAllLocks(Transaction* txn) override { locks_.UnlockAll(txn); }
    bool TakesReadLocks() const override { return false; }

    void OnBegin(Transaction* txn) override;
    void OnCommit(Transaction* txn) override;
    void OnAbort(Transaction* txn) override;
    void OnWrite(Transaction* txn, const RID& rid, WriteKind kind, bool before_exists, const std::string& before) override;
    bool ResolveRead(Transaction* txn, const RID& rid, bool latest_exists, const std::string& latest, std::string* out) override;

    LockManager& Locks() { return locks_; }       // write locks only
    void GarbageCollect();                          // drop versions no active snapshot can see
    std::size_t VersionCount();                     // old versions currently retained (tests / monitoring)
    std::size_t ChainCount();
    uint64_t LastCommitTs();

private:
    struct OldVersion {
        uint64_t begin_ts;  // commit timestamp of the transaction that created this version
        std::string data;
    };
    struct Chain {
        txn_id_t writer{0};                // transaction holding the uncommitted newest version, 0 if none
        uint64_t head_ts{0};               // commit timestamp of the heap's newest version (when writer == 0)
        std::vector<OldVersion> old;       // committed predecessors, newest first
    };
    struct WriteRec {
        RID rid;
        bool saved_preimage;  // false for inserts: there is nothing older to keep
    };

    void GcLocked();
    void EndTxnLocked(Transaction* txn);

    LockManager locks_;
    std::mutex mu_;
    std::unordered_map<RID, Chain, RIDHash> chains_;
    std::unordered_map<txn_id_t, std::vector<WriteRec>> writes_;
    std::multiset<uint64_t> active_snapshots_;
    uint64_t last_commit_ts_{0};
    std::size_t commits_since_gc_{0};
};

}  // namespace arbordb
