#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "db/database.h"
#include "stress_workloads.h"

using namespace arbordb;
using namespace std::chrono_literals;

namespace {
// Runs fn on a thread; if it does not finish in time the process exits (a hang would otherwise stall CI).
template <typename Fn>
void MustFinish(Fn fn, std::chrono::seconds limit = 10s) {
    auto p = std::make_shared<std::promise<void>>();
    std::thread t([fn, p] {
        fn();
        p->set_value();
    });
    if (p->get_future().wait_for(limit) != std::future_status::ready) {
        std::fprintf(stderr, "HANG: operation did not finish (reader blocked?)\n");
        std::_Exit(1);
    }
    t.join();
}

int64_t Read(StressEnv& env, Transaction* t, int acct) {
    std::string s;
    EXPECT_TRUE(env.heap->Get(t, env.accounts[acct], &s));
    return StressEnv::Value(s);
}
void Write(StressEnv& env, Transaction* t, int acct, int64_t v) {
    env.heap->Update(t, env.accounts[acct], StressEnv::Fmt("acct", acct, v));
}
size_t CountRows(StressEnv& env, Transaction* t) {
    size_t n = 0;
    TableHeap::Cursor c = env.heap->Begin();
    RID rid;
    std::string row;
    while (env.heap->Next(t, &c, &rid, &row)) n++;
    return n;
}
MVCCManager& Mvcc(StressEnv& env) { return static_cast<MVCCManager&>(*env.cc); }
bool Aborts(StressEnv& env, Transaction* t, int acct, int64_t v, std::string* why = nullptr) {
    try {
        Write(env, t, acct, v);
    } catch (const TransactionAbortedException& e) {
        if (why) *why = e.what();
        return true;
    }
    return false;
}
std::string NewDir(const std::string& n) { return "/tmp/arbor_mvcc_" + std::to_string(getpid()) + "_" + n; }
}  // namespace

// ------------------------------------------------------------ snapshot reads
TEST(MVCC, SnapshotReadIsRepeatableEvenAfterAnotherTransactionCommits) {
    StressEnv env(NewDir("repeat"), 2, 100, true);
    auto reader = env.tm->Begin();
    EXPECT_EQ(Read(env, reader.get(), 0), 100);
    auto writer = env.tm->Begin();
    Write(env, writer.get(), 0, 999);
    env.tm->Commit(writer.get());
    EXPECT_EQ(Read(env, reader.get(), 0), 100) << "snapshot must not move";
    env.tm->Commit(reader.get());
    auto later = env.tm->Begin();
    EXPECT_EQ(Read(env, later.get(), 0), 999);
    env.tm->Commit(later.get());
}

TEST(MVCC, EachSnapshotSeesTheVersionCurrentWhenItBegan) {
    StressEnv env(NewDir("chain"), 1, 100, true);
    auto s0 = env.tm->Begin();
    auto w1 = env.tm->Begin();
    Write(env, w1.get(), 0, 1);
    env.tm->Commit(w1.get());
    auto s1 = env.tm->Begin();
    auto w2 = env.tm->Begin();
    Write(env, w2.get(), 0, 2);
    env.tm->Commit(w2.get());
    auto s2 = env.tm->Begin();
    EXPECT_EQ(Read(env, s0.get(), 0), 100);
    EXPECT_EQ(Read(env, s1.get(), 0), 1);
    EXPECT_EQ(Read(env, s2.get(), 0), 2);
    for (auto* t : {s0.get(), s1.get(), s2.get()}) env.tm->Commit(t);
}

TEST(MVCC, ReadersNeverBlockOnAnUncommittedWriter) {
    StressEnv env(NewDir("noblock"), 2, 100, true);
    auto w = env.tm->Begin();
    Write(env, w.get(), 0, 999);  // holds the row's X lock, uncommitted
    int64_t seen = -1;
    MustFinish([&] {  // under strict 2PL this read would block until w ends
        auto r = env.tm->Begin();
        seen = Read(env, r.get(), 0);
        env.tm->Commit(r.get());
    });
    EXPECT_EQ(seen, 100) << "a reader must see the last committed value, never the uncommitted one";
    env.tm->Abort(w.get());
    auto r = env.tm->Begin();
    EXPECT_EQ(Read(env, r.get(), 0), 100);
    env.tm->Commit(r.get());
}

TEST(MVCC, TransactionSeesItsOwnWrites) {
    StressEnv env(NewDir("own"), 2, 100, true);
    auto t = env.tm->Begin();
    Write(env, t.get(), 0, 5);
    EXPECT_EQ(Read(env, t.get(), 0), 5);
    Write(env, t.get(), 0, 6);  // second write by the same txn
    EXPECT_EQ(Read(env, t.get(), 0), 6);
    RID mine = env.heap->Insert(t.get(), StressEnv::Fmt("acct", 9, 77));
    std::string s;
    EXPECT_TRUE(env.heap->Get(t.get(), mine, &s));
    EXPECT_TRUE(env.heap->Delete(t.get(), env.accounts[1]));
    EXPECT_FALSE(env.heap->Get(t.get(), env.accounts[1], &s));
    env.tm->Abort(t.get());
    auto r = env.tm->Begin();
    EXPECT_EQ(Read(env, r.get(), 0), 100);
    EXPECT_EQ(Read(env, r.get(), 1), 100);
    EXPECT_FALSE(env.heap->Get(r.get(), mine, &s));
    env.tm->Commit(r.get());
}

TEST(MVCC, ScansDoNotSeePhantomsInsertsOrDeletesFromLaterCommits) {
    StressEnv env(NewDir("phantom"), 4, 100, true);  // 4 accounts + the counter row = 5 rows
    auto reader = env.tm->Begin();
    EXPECT_EQ(CountRows(env, reader.get()), 5u);
    auto w = env.tm->Begin();
    env.heap->Insert(w.get(), StressEnv::Fmt("acct", 8, 1));
    env.heap->Insert(w.get(), StressEnv::Fmt("acct", 9, 1));
    env.heap->Delete(w.get(), env.accounts[0]);
    env.tm->Commit(w.get());
    EXPECT_EQ(CountRows(env, reader.get()), 5u) << "no phantoms, and the deleted row is still visible to the old snapshot";
    EXPECT_EQ(Read(env, reader.get(), 0), 100);
    env.tm->Commit(reader.get());
    auto later = env.tm->Begin();
    EXPECT_EQ(CountRows(env, later.get()), 6u);
    env.tm->Commit(later.get());
}

// ------------------------------------------------------------ write conflicts
TEST(MVCC, FirstCommitterWinsWhenTheOtherCommittedAfterOurSnapshot) {
    StressEnv env(NewDir("fcw"), 2, 100, true);
    auto t1 = env.tm->Begin(), t2 = env.tm->Begin();
    Write(env, t1.get(), 0, 111);
    env.tm->Commit(t1.get());
    std::string why;
    EXPECT_TRUE(Aborts(env, t2.get(), 0, 222, &why));
    EXPECT_NE(why.find("write-write conflict"), std::string::npos) << why;
    env.tm->Abort(t2.get());
    auto r = env.tm->Begin();
    EXPECT_EQ(Read(env, r.get(), 0), 111);
    env.tm->Commit(r.get());
}

TEST(MVCC, ConflictingWriterWaitsThenAbortsIfTheHolderCommits) {
    StressEnv env(NewDir("wait_commit"), 2, 100, true);
    auto t1 = env.tm->Begin(), t2 = env.tm->Begin();
    Write(env, t1.get(), 0, 111);
    std::atomic<bool> t2_done{false}, t2_aborted{false};
    std::thread th([&] {
        t2_aborted = Aborts(env, t2.get(), 0, 222);  // blocks on t1's X lock
        t2_done = true;
    });
    std::this_thread::sleep_for(150ms);
    EXPECT_FALSE(t2_done) << "writers on the same row must serialize";
    env.tm->Commit(t1.get());
    th.join();
    EXPECT_TRUE(t2_aborted) << "t1 committed after t2's snapshot: t2 lost the race";
    env.tm->Abort(t2.get());
}

TEST(MVCC, ConflictingWriterProceedsIfTheHolderAborts) {
    StressEnv env(NewDir("wait_abort"), 2, 100, true);
    auto t1 = env.tm->Begin(), t2 = env.tm->Begin();
    Write(env, t1.get(), 0, 111);
    std::atomic<bool> t2_aborted{true};
    std::thread th([&] { t2_aborted = Aborts(env, t2.get(), 0, 222); });
    std::this_thread::sleep_for(150ms);
    env.tm->Abort(t1.get());
    th.join();
    EXPECT_FALSE(t2_aborted);
    env.tm->Commit(t2.get());
    auto r = env.tm->Begin();
    EXPECT_EQ(Read(env, r.get(), 0), 222);
    env.tm->Commit(r.get());
}

TEST(MVCC, UpdateOfARowDeletedAfterOurSnapshotConflicts) {
    StressEnv env(NewDir("del_conflict"), 2, 100, true);
    auto t1 = env.tm->Begin(), t2 = env.tm->Begin();
    env.heap->Delete(t1.get(), env.accounts[0]);
    env.tm->Commit(t1.get());
    EXPECT_EQ(Read(env, t2.get(), 0), 100) << "t2 still sees the row";
    EXPECT_TRUE(Aborts(env, t2.get(), 0, 5));
    env.tm->Abort(t2.get());
}

// ------------------------------------------------------------ what snapshot isolation does NOT give you
// Two on-call doctors (rows a and b, value 1 = on call); the rule is "at least one stays on call".
// Each transaction checks the rule against its snapshot, then takes a DIFFERENT doctor off call.
static int RunWriteSkew(bool mvcc, int* commits_out) {
    StressEnv env(NewDir(mvcc ? "skew_mvcc" : "skew_2pl"), 2, 1, mvcc);
    auto t1 = env.tm->Begin(), t2 = env.tm->Begin();
    EXPECT_EQ(Read(env, t1.get(), 0) + Read(env, t1.get(), 1), 2);
    EXPECT_EQ(Read(env, t2.get(), 0) + Read(env, t2.get(), 1), 2);
    std::atomic<int> commits{0};
    auto go = [&](Transaction* t, int doctor) {
        try {
            Write(env, t, doctor, 0);
            env.tm->Commit(t);
            commits++;
        } catch (const TransactionAbortedException&) {
            env.tm->Abort(t);
        }
    };
    MustFinish([&] {
        std::thread a([&] { go(t1.get(), 0); }), b([&] { go(t2.get(), 1); });
        a.join();
        b.join();
    });
    auto r = env.tm->Begin();
    int on_call = static_cast<int>(Read(env, r.get(), 0) + Read(env, r.get(), 1));
    env.tm->Commit(r.get());
    *commits_out = commits;
    return on_call;
}

TEST(MVCC, WriteSkewIsPossibleUnderSnapshotIsolation) {
    int commits;
    int on_call = RunWriteSkew(/*mvcc=*/true, &commits);
    EXPECT_EQ(commits, 2) << "the two writes touch different rows, so snapshot isolation lets both commit";
    EXPECT_EQ(on_call, 0) << "...and the 'at least one doctor on call' invariant is now violated";
}

TEST(MVCC, StrictTwoPhaseLockingPreventsTheSameWriteSkew) {
    int commits;
    int on_call = RunWriteSkew(/*mvcc=*/false, &commits);
    EXPECT_EQ(commits, 1) << "S locks held to the end force a deadlock; one transaction is the victim";
    EXPECT_EQ(on_call, 1) << "the invariant holds";
}

// ------------------------------------------------------------ aborts and garbage collection
TEST(MVCC, AbortRestoresTheVersionChainAndLeavesNothingBehind) {
    StressEnv env(NewDir("abort_gc"), 2, 100, true);
    auto t = env.tm->Begin();
    Write(env, t.get(), 0, 5);
    env.heap->Insert(t.get(), StressEnv::Fmt("acct", 7, 1));
    env.tm->Abort(t.get());
    Mvcc(env).GarbageCollect();
    EXPECT_EQ(Mvcc(env).VersionCount(), 0u);
    EXPECT_EQ(Mvcc(env).ChainCount(), 0u);
    auto r = env.tm->Begin();
    EXPECT_EQ(Read(env, r.get(), 0), 100);
    EXPECT_EQ(CountRows(env, r.get()), 3u);
    env.tm->Commit(r.get());
}

TEST(MVCC, GarbageCollectionDropsVersionsNoSnapshotCanSee) {
    StressEnv env(NewDir("gc"), 1, 0, true);
    auto old_reader = env.tm->Begin();  // pins the original version
    for (int i = 1; i <= 20; i++) {
        auto w = env.tm->Begin();
        Write(env, w.get(), 0, i);
        env.tm->Commit(w.get());
    }
    Mvcc(env).GarbageCollect();
    EXPECT_GE(Mvcc(env).VersionCount(), 1u) << "the old reader still needs the version it began with";
    EXPECT_EQ(Read(env, old_reader.get(), 0), 0);
    env.tm->Commit(old_reader.get());
    Mvcc(env).GarbageCollect();
    EXPECT_EQ(Mvcc(env).VersionCount(), 0u);
    EXPECT_EQ(Mvcc(env).ChainCount(), 0u);
    auto r = env.tm->Begin();
    EXPECT_EQ(Read(env, r.get(), 0), 20);
    env.tm->Commit(r.get());
}

TEST(MVCC, GarbageCollectionKeepsOnlyTheVersionsIntermediateSnapshotsNeed) {
    StressEnv env(NewDir("gc_mid"), 1, 0, true);
    auto write = [&](int v) {
        auto w = env.tm->Begin();
        Write(env, w.get(), 0, v);
        env.tm->Commit(w.get());
    };
    write(1);
    auto mid = env.tm->Begin();  // sees 1
    write(2);
    write(3);
    write(4);
    Mvcc(env).GarbageCollect();
    EXPECT_EQ(Read(env, mid.get(), 0), 1);
    EXPECT_EQ(Mvcc(env).VersionCount(), 1u) << "only the version `mid` reads (value 1) must survive; 2 and 3 are unreachable";
    env.tm->Commit(mid.get());
}

// ------------------------------------------------------------ isolation under load
TEST(MVCC, MixedWorkloadKeepsTheBankInvariantAndLosesNoUpdates) {
    StressEnv env(NewDir("stress"), 10, 1000, true);
    StressStats st;
    StressResult r = RunMixedStress(env, st, 8, 1200);
    EXPECT_TRUE(r.ok) << r.report;
    EXPECT_EQ(st.audit_violations.load(), 0) << "an auditor saw a torn transfer";
    EXPECT_EQ(env.tm->ActiveCount(), 0u);
}

// ------------------------------------------------------------ through SQL
namespace {
DatabaseOptions Mvcc() {
    DatabaseOptions o;
    o.concurrency = ConcurrencyMode::kMVCC;
    return o;
}
std::vector<std::string> Rows(Session& s, const std::string& sql) { return s.Execute(sql).RowStrings(); }
std::string ErrorOf(Session& s, const std::string& sql) {
    try {
        s.Execute(sql);
    } catch (const DbError& e) {
        return e.what();
    }
    return "";
}
}  // namespace

TEST(MvccSql, TransactionKeepsItsSnapshotAcrossStatements) {
    Database db(NewDir("sql_snap"), Mvcc());
    Session a(&db), b(&db);
    a.ExecuteScript("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER); INSERT INTO t VALUES (1, 10), (2, 20);");
    a.Execute("BEGIN");
    EXPECT_EQ(Rows(a, "SELECT SUM(v) FROM t"), (std::vector<std::string>{"30"}));
    b.Execute("UPDATE t SET v = v + 1000");           // autocommit
    b.Execute("INSERT INTO t VALUES (3, 30)");
    b.Execute("DELETE FROM t WHERE id = 1");
    EXPECT_EQ(Rows(a, "SELECT SUM(v) FROM t"), (std::vector<std::string>{"30"})) << "a's snapshot must not change";
    EXPECT_EQ(Rows(a, "SELECT COUNT(*) FROM t"), (std::vector<std::string>{"2"}));
    a.Execute("COMMIT");
    EXPECT_EQ(Rows(a, "SELECT id, v FROM t ORDER BY id"), (std::vector<std::string>{"2|1020", "3|30"}));
}

TEST(MvccSql, WriteConflictIsReportedAndRollsTheTransactionBack) {
    Database db(NewDir("sql_conflict"), Mvcc());
    Session a(&db), b(&db);
    a.ExecuteScript("CREATE TABLE t (id INTEGER, v INTEGER); INSERT INTO t VALUES (1, 0);");
    a.Execute("BEGIN");
    a.Execute("SELECT * FROM t");           // snapshot is taken at BEGIN
    b.Execute("UPDATE t SET v = 5");        // commits first
    std::string err = ErrorOf(a, "UPDATE t SET v = 7");
    EXPECT_NE(err.find("write-write conflict"), std::string::npos) << err;
    EXPECT_FALSE(a.InTransaction());
    EXPECT_EQ(Rows(a, "SELECT v FROM t"), (std::vector<std::string>{"5"}));
}

TEST(MvccSql, ReadersDoNotWaitForWriters) {
    Database db(NewDir("sql_nowait"), Mvcc());
    Session writer(&db);
    writer.ExecuteScript("CREATE TABLE t (id INTEGER, v INTEGER); INSERT INTO t VALUES (1, 100);");
    writer.Execute("BEGIN");
    writer.Execute("UPDATE t SET v = 200");
    std::vector<std::string> seen;
    MustFinish([&] {
        Session reader(&db);
        seen = Rows(reader, "SELECT v FROM t");  // would block forever under 2PL until the writer ends
    });
    EXPECT_EQ(seen, (std::vector<std::string>{"100"}));
    writer.Execute("COMMIT");
}

TEST(MvccSql, IndexesStillEnforceUniquenessButPlansUseSequentialScans) {
    Database db(NewDir("sql_idx"), Mvcc());
    Session s(&db);
    s.ExecuteScript("CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER); INSERT INTO t VALUES (1, 1), (2, 2);");
    EXPECT_NE(ErrorOf(s, "INSERT INTO t VALUES (1, 9)").find("duplicate key"), std::string::npos);
    std::string plan;
    for (auto& r : s.Execute("EXPLAIN SELECT * FROM t WHERE id = 2").rows) plan += std::get<std::string>(r[0]) + "\n";
    EXPECT_EQ(plan.find("IndexScan"), std::string::npos) << plan;
    EXPECT_NE(plan.find("SeqScan"), std::string::npos);
    EXPECT_EQ(Rows(s, "SELECT v FROM t WHERE id = 2"), (std::vector<std::string>{"2"}));
}

TEST(MvccSql, CommittedWorkSurvivesACrashAndInFlightTransactionVanishes) {
    std::string dir = NewDir("sql_crash");
    {
        Database db(dir, Mvcc());
        Session s(&db);
        s.ExecuteScript(
            "CREATE TABLE acct (id INTEGER PRIMARY KEY, bal INTEGER);"
            "INSERT INTO acct VALUES (1, 100), (2, 200);"
            "UPDATE acct SET bal = 150 WHERE id = 1;");
        s.Execute("BEGIN");
        s.Execute("UPDATE acct SET bal = 0 WHERE id = 2");
        s.Execute("INSERT INTO acct VALUES (3, 300)");
        Session other(&db);
        other.Execute("CHECKPOINT");
        db.Crash();
        s.Abandon();
    }
    Database db(dir, Mvcc());  // the version store is gone, but the recovered heap only holds committed data
    Session s(&db);
    EXPECT_GT(db.LastRecovery().txns_undone, 0u);
    EXPECT_EQ(Rows(s, "SELECT id, bal FROM acct ORDER BY id"), (std::vector<std::string>{"1|150", "2|200"}));
}

TEST(MvccSql, ConcurrentTransfersConserveTheTotalAndAuditsNeverSeeATornTransfer) {
    Database db(NewDir("sql_bank"), Mvcc());
    {
        Session s(&db);
        s.Execute("CREATE TABLE acct (id INTEGER PRIMARY KEY, bal INTEGER)");
        for (int i = 0; i < 8; i++) s.Execute("INSERT INTO acct VALUES (" + std::to_string(i) + ", 1000)");
    }
    std::atomic<int> bad_audits{0}, committed{0}, conflicts{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 6; t++) {
        ts.emplace_back([&, t] {
            Session s(&db);
            std::mt19937 rng(t);
            for (int i = 0; i < 25; i++) {
                int from = rng() % 8, to = (from + 1 + rng() % 7) % 8, amt = 1 + rng() % 20;
                while (true) {
                    try {
                        s.Execute("BEGIN");
                        s.Execute("UPDATE acct SET bal = bal - " + std::to_string(amt) + " WHERE id = " + std::to_string(from));
                        s.Execute("UPDATE acct SET bal = bal + " + std::to_string(amt) + " WHERE id = " + std::to_string(to));
                        s.Execute("COMMIT");
                        committed++;
                        break;
                    } catch (const DbError& e) {
                        if (std::string(e.what()).find("transaction aborted") == std::string::npos) throw;
                        conflicts++;
                    }
                }
                if (i % 3 == 0 && Rows(s, "SELECT SUM(bal) FROM acct") != std::vector<std::string>{"8000"}) bad_audits++;
            }
        });
    }
    for (auto& t : ts) t.join();
    Session s(&db);
    EXPECT_EQ(bad_audits.load(), 0) << "snapshot reads must never observe half a transfer";
    EXPECT_EQ(committed.load(), 150);
    EXPECT_EQ(Rows(s, "SELECT SUM(bal), COUNT(*) FROM acct"), (std::vector<std::string>{"8000|8"}));
}
