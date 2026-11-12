#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "concurrency/transaction_manager.h"
#include "recovery/recovery_manager.h"
#include "storage/table/table_heap.h"

using namespace arbordb;
using Rows = std::vector<std::string>;

namespace {

// One "process lifetime" of the storage engine over files in `dir`. Crash() drops everything without
// flushing -- exactly what kill -9 does to the in-memory state (dirty pages and the unflushed log tail).
struct Engine {
    std::string dir;
    std::unique_ptr<DiskManager> dm;
    std::unique_ptr<LogManager> log;
    std::unique_ptr<BufferPoolManager> bpm;
    std::unique_ptr<TransactionManager> tm;
    std::unique_ptr<TableHeap> heap;
    RecoveryResult rec;

    Engine(const std::string& d, bool recover = true, std::size_t pool = 32) : dir(d) {
        dm = std::make_unique<DiskManager>(dir + "/data.db");
        log = std::make_unique<LogManager>(dir + "/wal.log");
        bpm = std::make_unique<BufferPoolManager>(pool, dm.get(), log.get());
        if (recover) FinishRecovery(RecoveryManager(log.get(), bpm.get()).Recover());
    }
    void FinishRecovery(RecoveryResult r) {
        rec = r;
        tm = std::make_unique<TransactionManager>(log.get(), bpm.get(), nullptr, r.max_txn_id + 1);
    }
    // The first heap page is page 1 (the first page ever allocated in a fresh data file).
    void OpenHeap(page_id_t first = 1) { heap = std::make_unique<TableHeap>(bpm.get(), log.get(), nullptr, first); }
    void CreateTable() { OpenHeap(TableHeap::CreateFirstPage(bpm.get())); }
    void Crash() {
        heap.reset();
        tm.reset();
        bpm.reset();
        log.reset();
        dm.reset();
    }
    Rows Dump() {  // transaction-less scan of all live tuples, sorted
        Rows rows;
        TableHeap::Cursor c = heap->Begin();
        RID rid;
        std::string t;
        while (heap->Next(nullptr, &c, &rid, &t)) rows.push_back(t);
        std::sort(rows.begin(), rows.end());
        return rows;
    }
};

std::string NewDir(const char* name) {
    std::string d = "/tmp/arbor_rec_" + std::to_string(getpid()) + "_" + name;
    std::string cmd = "rm -rf " + d + " && mkdir -p " + d;
    EXPECT_EQ(system(cmd.c_str()), 0);
    return d;
}
Rows Sorted(Rows r) {
    std::sort(r.begin(), r.end());
    return r;
}
}  // namespace

// ---------------- log records and the log file ----------------

TEST(LogRecord, SerializationRoundTripsEveryType) {
    std::vector<LogRecord> recs;
    for (LogType t : {LogType::kBegin, LogType::kCommit, LogType::kAbort, LogType::kEnd}) {
        LogRecord r;
        r.type = t; r.lsn = 5; r.prev_lsn = 4; r.txn_id = 9;
        recs.push_back(r);
    }
    LogRecord u;
    u.type = LogType::kUpdate; u.lsn = 7; u.prev_lsn = 5; u.txn_id = 9;
    u.op.kind = PageOpKind::kUpdate; u.op.page_id = 3; u.op.slot = 11; u.op.before = "old"; u.op.after = "newer";
    recs.push_back(u);
    LogRecord c = u;
    c.type = LogType::kClr; c.undo_next_lsn = 4; c.op = u.op.Inverse();
    recs.push_back(c);
    LogRecord ck;
    ck.type = LogType::kCheckpoint; ck.lsn = 20; ck.txn_id = kInvalidTxnId; ck.redo_start_lsn = 15;
    ck.active_txns = {{3, 12}, {4, 19}};
    recs.push_back(ck);
    for (auto& r : recs) {
        std::string buf;
        SerializeLogRecord(r, &buf);
        LogRecord back;
        ASSERT_TRUE(DeserializeLogRecord(buf.data(), buf.size(), &back));
        EXPECT_EQ(back.type, r.type);
        EXPECT_EQ(back.lsn, r.lsn);
        EXPECT_EQ(back.prev_lsn, r.prev_lsn);
        EXPECT_EQ(back.txn_id, r.txn_id);
        EXPECT_EQ(back.op.before, r.op.before);
        EXPECT_EQ(back.op.after, r.op.after);
        EXPECT_EQ(back.op.slot, r.op.slot);
        EXPECT_EQ(back.undo_next_lsn, r.undo_next_lsn);
        EXPECT_EQ(back.redo_start_lsn, r.redo_start_lsn);
        EXPECT_EQ(back.active_txns, r.active_txns);
        EXPECT_FALSE(DeserializeLogRecord(buf.data(), buf.size() - 1, &back));  // truncated -> rejected
    }
}

TEST(LogRecord, InverseOfInverseAndRedoOnlyOps) {
    PageOp ins;
    ins.kind = PageOpKind::kInsert;
    EXPECT_EQ(ins.Inverse().kind, PageOpKind::kDelete);
    PageOp del;
    del.kind = PageOpKind::kDelete;
    EXPECT_EQ(del.Inverse().kind, PageOpKind::kUndelete);
    PageOp upd;
    upd.kind = PageOpKind::kUpdate; upd.before = "a"; upd.after = "b";
    EXPECT_EQ(upd.Inverse().before, "b");
    EXPECT_EQ(upd.Inverse().after, "a");
    PageOp init;
    init.kind = PageOpKind::kInitPage;
    EXPECT_FALSE(init.Undoable());
    EXPECT_THROW(init.Inverse(), std::logic_error);
}

TEST(LogManager, ReopenKeepsFlushedRecordsAndDropsUnflushedOnes) {
    std::string d = NewDir("logmgr");
    {
        LogManager lm(d + "/wal.log");
        LogRecord r;
        r.type = LogType::kBegin; r.txn_id = 1;
        EXPECT_EQ(lm.Append(&r), 1);
        r.txn_id = 2;
        EXPECT_EQ(lm.Append(&r), 2);
        lm.Flush();
        EXPECT_EQ(lm.FlushedLsn(), 2);
        r.txn_id = 3;
        lm.Append(&r);  // buffered only; lost when the object is dropped
    }
    LogManager lm(d + "/wal.log");
    EXPECT_EQ(lm.LastLsn(), 2);
    LogRecord r;
    ASSERT_TRUE(lm.ReadRecord(2, &r));
    EXPECT_EQ(r.txn_id, 2);
    EXPECT_FALSE(lm.ReadRecord(3, &r));
    r.type = LogType::kEnd;
    EXPECT_EQ(lm.Append(&r), 3);  // numbering continues
}

TEST(LogManager, TornTailIsTruncatedOnOpen) {
    std::string d = NewDir("torn");
    {
        LogManager lm(d + "/wal.log");
        LogRecord r;
        r.type = LogType::kBegin; r.txn_id = 1;
        lm.Append(&r);
        lm.Append(&r);
        lm.Flush();
    }
    {
        std::ofstream f(d + "/wal.log", std::ios::app | std::ios::binary);
        f.write("\x40\x00\x00\x00garbage-half-record", 22);  // length says 64 bytes, only a few follow
    }
    LogManager lm(d + "/wal.log");
    EXPECT_EQ(lm.LastLsn(), 2);
    LogRecord r;
    r.type = LogType::kCommit; r.txn_id = 1;
    EXPECT_EQ(lm.Append(&r), 3);
    lm.Flush();
    LogManager lm2(d + "/wal.log");
    EXPECT_EQ(lm2.LastLsn(), 3);
}

// ---------------- write-ahead rule ----------------

TEST(WriteAheadRule, PageFlushForcesLogFlushFirst) {
    std::string d = NewDir("wal");
    Engine e(d);
    e.CreateTable();
    auto txn = e.tm->Begin();
    e.heap->Insert(txn.get(), "row");
    lsn_t last = e.log->LastLsn();
    EXPECT_LT(e.log->FlushedLsn(), last);  // nothing flushed the log yet (no commit)
    e.bpm->FlushAll();                      // must push the log through the page's LSN first
    EXPECT_GE(e.log->FlushedLsn(), last);
}

// ---------------- the three ARIES phases ----------------

TEST(Recovery, RedoRestoresCommittedWorkThatNeverReachedDataPages) {
    std::string d = NewDir("redo");
    {
        Engine e(d);
        e.CreateTable();
        auto t = e.tm->Begin();
        for (int i = 0; i < 50; i++) e.heap->Insert(t.get(), "row-" + std::to_string(i));
        e.tm->Commit(t.get());  // log durable, data pages still only in memory
        e.Crash();
    }
    Engine e(d);
    EXPECT_GT(e.rec.records_redone, 0u);
    EXPECT_EQ(e.rec.txns_undone, 0u);
    e.OpenHeap();
    Rows expect;
    for (int i = 0; i < 50; i++) expect.push_back("row-" + std::to_string(i));
    EXPECT_EQ(e.Dump(), Sorted(expect));
}

TEST(Recovery, UndoRemovesUncommittedWorkThatReachedDisk) {
    std::string d = NewDir("undo");
    {
        Engine e(d);
        e.CreateTable();
        auto base = e.tm->Begin();
        RID keep = e.heap->Insert(base.get(), "keep");
        RID gone = e.heap->Insert(base.get(), "deleted-later");
        e.heap->Insert(base.get(), "untouched");
        e.tm->Commit(base.get());
        auto t = e.tm->Begin();
        e.heap->Insert(t.get(), "uncommitted-1");
        e.heap->Insert(t.get(), "uncommitted-2");
        e.heap->Update(t.get(), keep, "KEEP-CHANGED");
        e.heap->Delete(t.get(), gone);
        e.bpm->FlushAll();  // uncommitted changes are now ON DISK (WAL rule flushes the log too)
        e.Crash();
    }
    Engine e(d);
    EXPECT_EQ(e.rec.txns_undone, 1u);
    EXPECT_GT(e.rec.clrs_written, 0u);
    e.OpenHeap();
    EXPECT_EQ(e.Dump(), Sorted({"keep", "deleted-later", "untouched"}));
}

TEST(Recovery, InterleavedTransactionsOnTheSamePage) {
    std::string d = NewDir("mixed");
    {
        Engine e(d);
        e.CreateTable();
        auto t1 = e.tm->Begin();
        auto t2 = e.tm->Begin();
        e.heap->Insert(t1.get(), "a-from-t1");
        e.heap->Insert(t2.get(), "b-from-t2");
        e.heap->Insert(t1.get(), "c-from-t1");
        e.heap->Insert(t2.get(), "d-from-t2");
        e.tm->Commit(t1.get());
        e.bpm->FlushAll();
        e.Crash();  // t2 was in flight
    }
    Engine e(d);
    EXPECT_EQ(e.rec.txns_undone, 1u);
    e.OpenHeap();
    EXPECT_EQ(e.Dump(), Sorted({"a-from-t1", "c-from-t1"}));  // t2's rows gone, t1's rows (same page) intact
}

TEST(Recovery, RedoIsGatedByPageLsnSoFlushedPagesAreNotReapplied) {
    std::string d = NewDir("gate");
    {
        Engine e(d);
        e.CreateTable();
        auto t = e.tm->Begin();
        for (int i = 0; i < 10; i++) e.heap->Insert(t.get(), "r" + std::to_string(i));
        e.tm->Commit(t.get());
        e.bpm->FlushAll();  // pages already reflect every logged change
        e.Crash();
    }
    Engine e(d);
    EXPECT_EQ(e.rec.records_redone, 0u);
    EXPECT_GT(e.rec.records_skipped, 0u);
    e.OpenHeap();
    EXPECT_EQ(e.Dump().size(), 10u);  // no duplicates from a double apply
}

TEST(Recovery, RecoveryIsIdempotentAcrossRepeatedCrashes) {
    std::string d = NewDir("idem");
    {
        Engine e(d);
        e.CreateTable();
        auto t1 = e.tm->Begin();
        e.heap->Insert(t1.get(), "committed");
        e.tm->Commit(t1.get());
        auto t2 = e.tm->Begin();
        e.heap->Insert(t2.get(), "loser");
        e.bpm->FlushAll();
        e.Crash();
    }
    for (int round = 0; round < 3; round++) {
        Engine e(d);  // recovers, then crashes again without checkpointing or flushing data pages
        e.OpenHeap();
        EXPECT_EQ(e.Dump(), Sorted({"committed"})) << "round " << round;
        if (round > 0) {
            EXPECT_EQ(e.rec.txns_undone, 0u) << "loser already ended in round 0";
        }
        e.Crash();
    }
}

TEST(Recovery, CrashDuringUndoResumesWithoutUndoingTwice) {
    std::string d = NewDir("undocrash");
    {
        Engine e(d);
        e.CreateTable();
        auto base = e.tm->Begin();
        e.heap->Insert(base.get(), "baseline");
        e.tm->Commit(base.get());
        auto t = e.tm->Begin();
        for (int i = 0; i < 10; i++) e.heap->Insert(t.get(), "loser-" + std::to_string(i));
        e.bpm->FlushAll();
        e.Crash();
    }
    {  // first recovery attempt dies after 4 compensations
        Engine e(d, /*recover=*/false);
        RecoveryManager rm(e.log.get(), e.bpm.get());
        EXPECT_THROW(rm.Recover(/*crash_after_clrs=*/4), SimulatedCrash);
        e.log->Flush();
        e.bpm->FlushAll();  // the partial undo is durable, both in the log and on the pages
        e.Crash();
    }
    Engine e(d);  // second recovery must finish the job using the CLRs' undo_next pointers
    EXPECT_EQ(e.rec.clrs_written, 6u) << "10 inserts - 4 already compensated";
    e.OpenHeap();
    EXPECT_EQ(e.Dump(), Sorted({"baseline"}));
}

TEST(Recovery, CheckpointBoundsAnalysisButUndoStillReachesBeforeIt) {
    std::string d = NewDir("ckpt");
    lsn_t ckpt_redo_start;
    {
        Engine e(d);
        e.CreateTable();
        auto a = e.tm->Begin();
        for (int i = 0; i < 5; i++) e.heap->Insert(a.get(), "A" + std::to_string(i));
        e.tm->Commit(a.get());
        auto b = e.tm->Begin();  // in flight across the checkpoint
        e.heap->Insert(b.get(), "B-before-ckpt");
        e.tm->Checkpoint();
        LogRecord ck;
        ASSERT_TRUE(e.log->ReadRecord(e.log->CheckpointLsn(), &ck));
        ckpt_redo_start = ck.redo_start_lsn;
        ASSERT_EQ(ck.active_txns.size(), 1u);
        e.heap->Insert(b.get(), "B-after-ckpt");
        auto c = e.tm->Begin();
        e.heap->Insert(c.get(), "C");
        e.tm->Commit(c.get());
        e.Crash();
    }
    Engine e(d);
    EXPECT_EQ(e.rec.analysis_start_lsn, ckpt_redo_start);
    EXPECT_GT(e.rec.analysis_start_lsn, 1);
    EXPECT_EQ(e.rec.txns_undone, 1u);
    e.OpenHeap();
    EXPECT_EQ(e.Dump(), Sorted({"A0", "A1", "A2", "A3", "A4", "C"}));
}

TEST(Recovery, MultiPageTableGrowthIsRedoneIncludingPageLinks) {
    std::string d = NewDir("multipage");
    Rows expect;
    {
        Engine e(d);
        e.CreateTable();
        auto t = e.tm->Begin();
        for (int i = 0; i < 500; i++) {
            std::string row = "row-" + std::to_string(i) + std::string(100, 'x');
            e.heap->Insert(t.get(), row);
            expect.push_back(row);
        }
        e.tm->Commit(t.get());
        EXPECT_GT(e.heap->NumPages(), 10u);
        e.Crash();  // no data page ever flushed except page 1
    }
    Engine e(d);
    e.OpenHeap();
    EXPECT_GT(e.heap->NumPages(), 10u);
    EXPECT_EQ(e.Dump(), Sorted(expect));
}

TEST(Recovery, TransactionIdsContinueAfterRecovery) {
    std::string d = NewDir("txnid");
    {
        Engine e(d);
        e.CreateTable();
        for (int i = 0; i < 3; i++) {
            auto t = e.tm->Begin();
            e.heap->Insert(t.get(), "x" + std::to_string(i));
            e.tm->Commit(t.get());
        }
        e.Crash();
    }
    Engine e(d);
    EXPECT_EQ(e.rec.max_txn_id, 3);
    EXPECT_EQ(e.tm->Begin()->Id(), 4);
}

// ---------------- normal (non-crash) rollback ----------------

TEST(Abort, RollsBackInsertUpdateDeleteAndLeavesNoTrace) {
    std::string d = NewDir("abort");
    Engine e(d);
    e.CreateTable();
    auto base = e.tm->Begin();
    RID r1 = e.heap->Insert(base.get(), "one");
    RID r2 = e.heap->Insert(base.get(), "two-two");
    e.tm->Commit(base.get());
    auto t = e.tm->Begin();
    e.heap->Insert(t.get(), "new");
    e.heap->Update(t.get(), r1, "ONE-LONGER-THAN-BEFORE-BY-A-LOT");  // grows -> may relocate
    e.heap->Delete(t.get(), r2);
    EXPECT_EQ(e.Dump().size(), 2u);
    e.tm->Abort(t.get());
    EXPECT_EQ(t->State(), TxnState::kAborted);
    EXPECT_EQ(e.Dump(), Sorted({"one", "two-two"}));
    // The abort completed (END logged); crashing now and recovering must agree.
    e.Crash();
    Engine e2(d);
    EXPECT_EQ(e2.rec.txns_undone, 0u);
    e2.OpenHeap();
    EXPECT_EQ(e2.Dump(), Sorted({"one", "two-two"}));
}

TEST(Abort, ShrinkingUpdateIsUndoneToFullLength) {
    std::string d = NewDir("abort_shrink");
    Engine e(d);
    e.CreateTable();
    auto base = e.tm->Begin();
    RID r = e.heap->Insert(base.get(), "a-fairly-long-original-value");
    e.tm->Commit(base.get());
    auto t = e.tm->Begin();
    e.heap->Update(t.get(), r, "tiny");
    e.tm->Abort(t.get());
    std::string out;
    ASSERT_TRUE(e.heap->Get(nullptr, r, &out));
    EXPECT_EQ(out, "a-fairly-long-original-value");
}

TEST(TableHeap, RejectsOversizeAndEmptyTuples) {
    std::string d = NewDir("heap_size");
    Engine e(d);
    e.CreateTable();
    auto t = e.tm->Begin();
    EXPECT_THROW(e.heap->Insert(t.get(), std::string(TableHeap::kMaxTupleSize + 1, 'x')), std::length_error);
    EXPECT_THROW(e.heap->Insert(t.get(), ""), std::length_error);
    EXPECT_NO_THROW(e.heap->Insert(t.get(), std::string(TableHeap::kMaxTupleSize, 'x')));
}

TEST(TableHeap, DeleteOfMissingOrDeletedTupleReturnsFalse) {
    std::string d = NewDir("heap_missing");
    Engine e(d);
    e.CreateTable();
    auto t = e.tm->Begin();
    RID r = e.heap->Insert(t.get(), "x");
    EXPECT_TRUE(e.heap->Delete(t.get(), r));
    EXPECT_FALSE(e.heap->Delete(t.get(), r));
    EXPECT_FALSE(e.heap->Delete(t.get(), RID{1, 999}));
    EXPECT_FALSE(e.heap->Update(t.get(), r, "y").has_value());
    std::string out;
    EXPECT_FALSE(e.heap->Get(t.get(), r, &out));
}
