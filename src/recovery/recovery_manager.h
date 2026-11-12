#pragma once

#include <stdexcept>

#include "recovery/log_manager.h"

namespace arbordb {

struct RecoveryResult {
    txn_id_t max_txn_id{0};
    lsn_t analysis_start_lsn{1};  // where the log scan began (checkpoint's redo_start_lsn, or 1)
    std::size_t records_redone{0};   // page ops actually re-applied (LSN gate passed)
    std::size_t records_skipped{0};  // page ops already reflected on disk
    std::size_t txns_undone{0};
    std::size_t clrs_written{0};
};

// Thrown by the test hook below to simulate a crash in the middle of the Undo phase.
struct SimulatedCrash : std::runtime_error {
    SimulatedCrash() : std::runtime_error("simulated crash during undo") {}
};

// ARIES restart recovery. Run once at startup, before any new transaction.
//   1. Analysis: scan the log from the last checkpoint's redo_start_lsn, rebuilding the transaction
//      table (txns with no COMMIT/END = losers) and the dirty page table.
//   2. Redo: repeat history -- re-apply every logged page change (including CLRs) whose page LSN is
//      older than the record, for committed and uncommitted transactions alike.
//   3. Undo: roll back every loser by walking its prev_lsn chain, applying inverse operations and
//      logging a CLR for each, so a crash during undo is itself recoverable and never undoes twice.
class RecoveryManager {
public:
    RecoveryManager(LogManager* log, BufferPoolManager* bpm) : log_(log), bpm_(bpm) {}

    // crash_after_clrs >= 1: throw SimulatedCrash right after that many CLRs were written (tests only).
    RecoveryResult Recover(int crash_after_clrs = -1);

    // Roll back `txn`: undo the records reachable from `undo_from`, writing CLRs chained after
    // `*chain_lsn` (updated). Shared by Recover() and TransactionManager::Abort(). Returns CLR count.
    static std::size_t UndoChain(LogManager* log, BufferPoolManager* bpm, txn_id_t txn, lsn_t undo_from,
                                 lsn_t* chain_lsn, int* crash_budget = nullptr);

private:
    LogManager* log_;
    BufferPoolManager* bpm_;
};

}  // namespace arbordb
