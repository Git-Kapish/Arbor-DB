#include "concurrency/mvcc_manager.h"

namespace arbordb {

void MVCCManager::OnBegin(Transaction* txn) {
    std::lock_guard<std::mutex> g(mu_);
    txn->snapshot_ts = last_commit_ts_;
    active_snapshots_.insert(last_commit_ts_);
}

void MVCCManager::EndTxnLocked(Transaction* txn) {
    auto it = active_snapshots_.find(txn->snapshot_ts);
    if (it != active_snapshots_.end()) active_snapshots_.erase(it);
    writes_.erase(txn->Id());
}

// Reads take no locks. Writes take an X row lock, then apply first-committer-wins: if the row's newest
// committed version is newer than this transaction's snapshot, someone committed a change we cannot see.
bool MVCCManager::AcquireLock(Transaction* txn, const RID& rid, LockMode mode) {
    if (mode == LockMode::kShared) return true;
    if (!locks_.Lock(txn, rid, LockMode::kExclusive)) return false;  // deadlock victim (default reason)
    std::lock_guard<std::mutex> g(mu_);
    auto it = chains_.find(rid);
    if (it != chains_.end() && it->second.writer == 0 && it->second.head_ts > txn->snapshot_ts) {
        txn->abort_reason = "write-write conflict: row was changed by a transaction that committed after this one's snapshot";
        return false;
    }
    return true;
}

// Called under the page latch before the heap tuple changes: remember what is about to be overwritten.
void MVCCManager::OnWrite(Transaction* txn, const RID& rid, WriteKind kind, bool /*before_exists*/, const std::string& before) {
    std::lock_guard<std::mutex> g(mu_);
    Chain& c = chains_[rid];
    if (c.writer == txn->Id()) return;  // this transaction already saved the pre-image on its first write
    bool saved = false;
    if (kind != WriteKind::kInsert) {
        c.old.insert(c.old.begin(), OldVersion{c.head_ts, before});
        saved = true;
    }
    c.writer = txn->Id();
    writes_[txn->Id()].push_back(WriteRec{rid, saved});
}

void MVCCManager::OnCommit(Transaction* txn) {
    std::lock_guard<std::mutex> g(mu_);
    auto w = writes_.find(txn->Id());
    if (w != writes_.end() && !w->second.empty()) {
        uint64_t ts = ++last_commit_ts_;  // assigned and published atomically with the stamping below
        for (const WriteRec& rec : w->second) {
            Chain& c = chains_[rec.rid];
            c.writer = 0;
            c.head_ts = ts;
        }
    }
    EndTxnLocked(txn);
    if (++commits_since_gc_ >= kGcInterval) GcLocked();
}

// The heap rows were already restored by the ARIES undo; put the version chains back the same way.
void MVCCManager::OnAbort(Transaction* txn) {
    std::lock_guard<std::mutex> g(mu_);
    auto w = writes_.find(txn->Id());
    if (w != writes_.end()) {
        for (const WriteRec& rec : w->second) {
            auto it = chains_.find(rec.rid);
            if (it == chains_.end()) continue;
            Chain& c = it->second;
            if (rec.saved_preimage) {
                c.head_ts = c.old.front().begin_ts;
                c.old.erase(c.old.begin());
                c.writer = 0;
            } else {
                chains_.erase(it);  // an aborted insert leaves only a tombstone behind
            }
        }
    }
    EndTxnLocked(txn);
}

// `latest` is the newest version stored in the heap. Pick the version `txn` is allowed to see.
bool MVCCManager::ResolveRead(Transaction* txn, const RID& rid, bool latest_exists, const std::string& latest, std::string* out) {
    auto take_latest = [&] {
        if (latest_exists) *out = latest;
        return latest_exists;
    };
    std::lock_guard<std::mutex> g(mu_);
    auto it = chains_.find(rid);
    if (it == chains_.end()) return take_latest();  // untouched since startup: committed at ts 0, visible to all
    const Chain& c = it->second;
    if (c.writer == txn->Id()) return take_latest();  // our own write
    uint64_t snap = txn->snapshot_ts;
    if (c.writer == 0 && c.head_ts <= snap) return take_latest();
    // The newest version is uncommitted (someone else's) or too new: walk back to the newest one we may see.
    for (const OldVersion& v : c.old) {
        if (v.begin_ts <= snap) {
            *out = v.data;
            return true;
        }
    }
    return false;  // the row did not exist yet at our snapshot
}

// Keep an old version only if some active snapshot can still read it, i.e. a snapshot s with
// begin_ts <= s < (begin_ts of the next newer version). New snapshots always start at the latest commit
// timestamp, so they never need anything older than the newest committed version -- except while a writer
// is in flight, when the newest COMMITTED version is the pre-image `old[0]`, which is always kept.
void MVCCManager::GcLocked() {
    commits_since_gc_ = 0;
    for (auto it = chains_.begin(); it != chains_.end();) {
        Chain& c = it->second;
        std::vector<OldVersion> kept;
        uint64_t upper = c.writer == 0 ? c.head_ts : UINT64_MAX;  // end of the validity interval of the version below
        for (std::size_t i = 0; i < c.old.size(); i++) {
            auto s = active_snapshots_.lower_bound(c.old[i].begin_ts);
            bool needed = (i == 0 && c.writer != 0) || (s != active_snapshots_.end() && *s < upper);
            uint64_t begin = c.old[i].begin_ts;
            if (needed) kept.push_back(std::move(c.old[i]));
            upper = begin;
        }
        c.old = std::move(kept);
        // With no old versions left the chain only exists to say "newer than your snapshot => not visible yet".
        bool head_visible_to_all = active_snapshots_.empty() || *active_snapshots_.begin() >= c.head_ts;
        if (c.writer == 0 && c.old.empty() && head_visible_to_all) it = chains_.erase(it);
        else ++it;
    }
}

void MVCCManager::GarbageCollect() {
    std::lock_guard<std::mutex> g(mu_);
    GcLocked();
}

std::size_t MVCCManager::VersionCount() {
    std::lock_guard<std::mutex> g(mu_);
    std::size_t n = 0;
    for (auto& [rid, c] : chains_) n += c.old.size();
    return n;
}

std::size_t MVCCManager::ChainCount() {
    std::lock_guard<std::mutex> g(mu_);
    return chains_.size();
}

uint64_t MVCCManager::LastCommitTs() {
    std::lock_guard<std::mutex> g(mu_);
    return last_commit_ts_;
}

}  // namespace arbordb
