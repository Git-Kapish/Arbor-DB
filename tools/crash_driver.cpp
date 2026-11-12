// Driver for scripts/crash_recovery_test.sh. Uses the storage engine directly (heap + WAL + recovery).
//   crash_driver setup  <dir>   build a state with committed AND in-flight work, print READY, then block (script kill -9s it)
//   crash_driver verify <dir>   open (runs recovery) and check committed work is present and in-flight work is absent
//   crash_driver churn  <dir>   commit small transactions forever, printing "ACK <n>" after each durable commit
//   crash_driver verify-churn <dir> <last_acked_n>   check every acked txn is fully present, no txn is partial
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "concurrency/transaction_manager.h"
#include "recovery/recovery_manager.h"
#include "storage/table/table_heap.h"

using namespace arbordb;

namespace {
struct Db {
    std::unique_ptr<DiskManager> dm;
    std::unique_ptr<LogManager> log;
    std::unique_ptr<BufferPoolManager> bpm;
    std::unique_ptr<TransactionManager> tm;
    std::unique_ptr<TableHeap> heap;
    RecoveryResult rec;

    // Opening runs ARIES recovery, exactly as the real engine does at startup.
    explicit Db(const std::string& dir, bool create) {
        dm = std::make_unique<DiskManager>(dir + "/data.db");
        log = std::make_unique<LogManager>(dir + "/wal.log");
        bpm = std::make_unique<BufferPoolManager>(64, dm.get(), log.get());
        rec = RecoveryManager(log.get(), bpm.get()).Recover();
        tm = std::make_unique<TransactionManager>(log.get(), bpm.get(), nullptr, rec.max_txn_id + 1);
        page_id_t first;
        if (create) {
            first = TableHeap::CreateFirstPage(bpm.get());
            std::ofstream(dir + "/table.meta") << first << "\n";
        } else {
            std::ifstream(dir + "/table.meta") >> first;
        }
        heap = std::make_unique<TableHeap>(bpm.get(), log.get(), nullptr, first);
    }
    std::multiset<std::string> Rows() {
        std::multiset<std::string> rows;
        auto c = heap->Begin();
        RID rid;
        std::string t;
        while (heap->Next(nullptr, &c, &rid, &t)) rows.insert(t);
        return rows;
    }
};

int Fail(const std::string& msg) {
    std::printf("FAIL: %s\n", msg.c_str());
    return 1;
}

int Setup(const std::string& dir) {
    Db db(dir, true);
    auto a = db.tm->Begin();
    std::vector<RID> rids;
    for (int i = 0; i < 100; i++) rids.push_back(db.heap->Insert(a.get(), "committed-" + std::to_string(i)));
    db.tm->Commit(a.get());  // durable in the log; data pages are NOT flushed

    auto b = db.tm->Begin();  // never commits
    for (int i = 0; i < 50; i++) db.heap->Insert(b.get(), "uncommitted-" + std::to_string(i));
    db.heap->Update(b.get(), rids[0], "CHANGED-BY-LOSER");
    db.heap->Delete(b.get(), rids[1]);
    db.bpm->FlushAll();  // the loser's changes are now on disk: only Undo can remove them
    for (int i = 0; i < 20; i++) db.heap->Insert(b.get(), "uncommitted-late-" + std::to_string(i));  // memory only
    std::printf("READY\n");
    std::fflush(stdout);
    for (;;) pause();  // wait for kill -9
}

int Verify(const std::string& dir) {
    Db db(dir, false);
    std::multiset<std::string> expect;
    for (int i = 0; i < 100; i++) expect.insert("committed-" + std::to_string(i));
    auto got = db.Rows();
    if (got != expect) {
        for (auto& r : got) if (!expect.count(r)) std::printf("unexpected row: %s\n", r.c_str());
        for (auto& r : expect) if (!got.count(r)) std::printf("missing row: %s\n", r.c_str());
        return Fail("table contents differ after recovery (" + std::to_string(got.size()) + " rows)");
    }
    std::printf("OK committed=100 uncommitted=0 (redone=%zu undone_txns=%zu clrs=%zu)\n", db.rec.records_redone,
                db.rec.txns_undone, db.rec.clrs_written);
    return 0;
}

int Churn(const std::string& dir) {
    Db db(dir, true);
    for (int n = 1;; n++) {
        auto t = db.tm->Begin();
        for (int k = 0; k < 5; k++) db.heap->Insert(t.get(), "t" + std::to_string(n) + "-" + std::to_string(k));
        if (n % 7 == 0) {  // an in-flight transaction that gets overtaken by the kill
            auto loser = db.tm->Begin();
            db.heap->Insert(loser.get(), "loser" + std::to_string(n));
            db.bpm->FlushAll();
            db.tm->Abort(loser.get());
        }
        db.tm->Commit(t.get());
        std::printf("ACK %d\n", n);
        std::fflush(stdout);
        if (n % 11 == 0) db.tm->Checkpoint();
        if (n % 5 == 0) db.bpm->FlushAll();
    }
}

int VerifyChurn(const std::string& dir, int acked) {
    Db db(dir, false);
    std::map<int, int> per_txn;
    for (auto& r : db.Rows()) {
        if (r[0] != 't') return Fail("row from a transaction that never committed: " + r);
        per_txn[std::atoi(r.c_str() + 1)]++;
    }
    for (int n = 1; n <= acked; n++)
        if (per_txn[n] != 5) return Fail("acknowledged txn " + std::to_string(n) + " has " + std::to_string(per_txn[n]) + "/5 rows");
    for (auto& [n, cnt] : per_txn)
        if (cnt != 5) return Fail("txn " + std::to_string(n) + " is partially present (" + std::to_string(cnt) + "/5): atomicity violated");
    std::printf("OK acked=%d recovered_txns=%zu\n", acked, per_txn.size());
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return Fail("usage: crash_driver <setup|verify|churn|verify-churn> <dir> [n]");
    std::string mode = argv[1], dir = argv[2];
    if (mode == "setup") return Setup(dir);
    if (mode == "verify") return Verify(dir);
    if (mode == "churn") return Churn(dir);
    if (mode == "verify-churn" && argc >= 4) return VerifyChurn(dir, std::atoi(argv[3]));
    return Fail("bad arguments");
}
