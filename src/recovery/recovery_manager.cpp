#include "recovery/recovery_manager.h"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace arbordb {

std::size_t RecoveryManager::UndoChain(LogManager* log, BufferPoolManager* bpm, txn_id_t txn, lsn_t undo_from,
                                       lsn_t* chain_lsn, int* crash_budget) {
    std::size_t clrs = 0;
    lsn_t cur = undo_from;
    while (cur != kInvalidLsn) {
        LogRecord r;
        if (!log->ReadRecord(cur, &r)) throw std::runtime_error("undo: missing log record " + std::to_string(cur));
        if (r.type == LogType::kUpdate && r.op.Undoable()) {
            PageOp inv = r.op.Inverse();
            Page* page = bpm->FetchPage(inv.page_id);
            if (!page) throw std::runtime_error("undo: buffer pool exhausted");
            page->WLatch();
            LogRecord clr;
            clr.type = LogType::kClr;
            clr.txn_id = txn;
            clr.prev_lsn = *chain_lsn;
            clr.op = inv;
            clr.undo_next_lsn = r.prev_lsn;  // skip this record next time
            lsn_t lsn = log->Append(&clr);
            *chain_lsn = lsn;
            ApplyPageOp(page, inv);
            page->SetLSN(lsn);
            page->WUnlatch();
            bpm->UnpinPage(inv.page_id, true);
            clrs++;
            if (crash_budget && *crash_budget > 0 && --*crash_budget == 0) throw SimulatedCrash();
            cur = r.prev_lsn;
        } else if (r.type == LogType::kClr) {
            cur = r.undo_next_lsn;  // already-undone work is never undone again
        } else {
            cur = r.prev_lsn;  // BEGIN, ABORT, redo-only page ops
        }
    }
    return clrs;
}

RecoveryResult RecoveryManager::Recover(int crash_after_clrs) {
    RecoveryResult res;

    // ---- 1. Analysis ----
    std::map<txn_id_t, lsn_t> att;  // active transaction table: txn -> last lsn
    lsn_t start = 1;
    if (lsn_t ck = log_->CheckpointLsn(); ck != 0) {
        LogRecord c;
        if (log_->ReadRecord(ck, &c) && c.type == LogType::kCheckpoint) {
            for (auto& [t, l] : c.active_txns) att[t] = l;
            start = c.redo_start_lsn;
        }
    }
    res.analysis_start_lsn = start;
    std::map<page_id_t, lsn_t> dpt;  // dirty page table: page -> recLSN (first record that may need redo)
    std::vector<LogRecord> records = log_->ScanFrom(start);
    for (const LogRecord& r : records) {
        res.max_txn_id = std::max(res.max_txn_id, r.txn_id);
        switch (r.type) {
            case LogType::kBegin:
            case LogType::kAbort:
            case LogType::kUpdate:
            case LogType::kClr: att[r.txn_id] = r.lsn; break;
            case LogType::kCommit:
            case LogType::kEnd: att.erase(r.txn_id); break;
            case LogType::kCheckpoint: break;
        }
        if (r.ModifiesPage()) dpt.emplace(r.op.page_id, r.lsn);
    }
    for (auto& [t, l] : att) res.max_txn_id = std::max(res.max_txn_id, t);

    // ---- 2. Redo ----
    for (const LogRecord& r : records) {
        if (!r.ModifiesPage()) continue;
        auto it = dpt.find(r.op.page_id);
        if (it == dpt.end() || r.lsn < it->second) continue;
        bpm_->Disk()->EnsureAllocated(r.op.page_id);  // the file may be shorter than the log implies
        Page* page = bpm_->FetchPage(r.op.page_id);
        if (!page) throw std::runtime_error("redo: buffer pool exhausted");
        page->WLatch();
        if (page->GetLSN() < r.lsn) {  // the page does not yet reflect this change
            ApplyPageOp(page, r.op);
            page->SetLSN(r.lsn);
            res.records_redone++;
            page->WUnlatch();
            bpm_->UnpinPage(r.op.page_id, true);
        } else {
            res.records_skipped++;
            page->WUnlatch();
            bpm_->UnpinPage(r.op.page_id, false);
        }
    }

    // ---- 3. Undo ----
    int budget = crash_after_clrs;
    for (auto& [txn, last_lsn] : att) {  // std::map: deterministic order
        lsn_t chain = last_lsn;
        res.clrs_written += UndoChain(log_, bpm_, txn, last_lsn, &chain, crash_after_clrs >= 1 ? &budget : nullptr);
        LogRecord end;
        end.type = LogType::kEnd;
        end.txn_id = txn;
        end.prev_lsn = chain;
        log_->Append(&end);
        res.txns_undone++;
    }
    log_->Flush();
    return res;
}

}  // namespace arbordb
