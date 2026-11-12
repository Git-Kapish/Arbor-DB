#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "stress_workloads.h"

using namespace arbordb;
using namespace std::chrono_literals;

namespace {
RID Row(int n) { return RID{1, static_cast<uint32_t>(n)}; }

// Joins a thread, but fails loudly (instead of hanging the suite) if a deadlock was NOT resolved.
struct Joiner {
    std::vector<std::thread> ts;
    std::vector<std::shared_ptr<std::promise<void>>> done;
    void Spawn(std::function<void()> fn) {
        auto p = std::make_shared<std::promise<void>>();
        done.push_back(p);
        ts.emplace_back([fn, p] {
            fn();
            p->set_value();
        });
    }
    void JoinAll(std::chrono::seconds limit = 10s) {
        for (auto& p : done) {
            if (p->get_future().wait_for(limit) != std::future_status::ready) {
                std::fprintf(stderr, "HANG: a lock request never returned (unresolved deadlock?)\n");
                std::_Exit(1);
            }
        }
        for (auto& t : ts) t.join();
    }
};
void Pause() { std::this_thread::sleep_for(150ms); }  // only used to assert that something stays blocked
}  // namespace

TEST(LockManager, SharedLocksAreCompatible) {
    LockManager lm;
    Transaction t1(1), t2(2);
    EXPECT_TRUE(lm.Lock(&t1, Row(0), LockMode::kShared));
    EXPECT_TRUE(lm.Lock(&t2, Row(0), LockMode::kShared));
    lm.UnlockAll(&t1);
    lm.UnlockAll(&t2);
}

TEST(LockManager, ExclusiveBlocksReadersAndWritersUntilRelease) {
    for (LockMode other : {LockMode::kShared, LockMode::kExclusive}) {
        LockManager lm;
        Transaction t1(1), t2(2);
        ASSERT_TRUE(lm.Lock(&t1, Row(0), LockMode::kExclusive));
        std::atomic<bool> granted{false};
        Joiner j;
        j.Spawn([&] {
            EXPECT_TRUE(lm.Lock(&t2, Row(0), other));
            granted = true;
        });
        Pause();
        EXPECT_FALSE(granted) << "must block while another txn holds X";
        lm.UnlockAll(&t1);
        j.JoinAll();
        EXPECT_TRUE(granted);
        lm.UnlockAll(&t2);
    }
}

TEST(LockManager, SharedHolderBlocksWriter) {
    LockManager lm;
    Transaction t1(1), t2(2);
    ASSERT_TRUE(lm.Lock(&t1, Row(0), LockMode::kShared));
    std::atomic<bool> granted{false};
    Joiner j;
    j.Spawn([&] {
        lm.Lock(&t2, Row(0), LockMode::kExclusive);
        granted = true;
    });
    Pause();
    EXPECT_FALSE(granted);
    lm.UnlockAll(&t1);
    j.JoinAll();
    EXPECT_TRUE(granted);
}

TEST(LockManager, ReentrantRequestsAndSoleHolderUpgrade) {
    LockManager lm;
    Transaction t1(1);
    EXPECT_TRUE(lm.Lock(&t1, Row(0), LockMode::kShared));
    EXPECT_TRUE(lm.Lock(&t1, Row(0), LockMode::kShared));
    EXPECT_TRUE(lm.Lock(&t1, Row(0), LockMode::kExclusive));  // sole holder: upgrade is immediate
    EXPECT_TRUE(lm.Lock(&t1, Row(0), LockMode::kShared));     // X already covers S
    EXPECT_EQ(lm.LockCount(1), 1u);
    lm.UnlockAll(&t1);
    EXPECT_EQ(lm.LockCount(1), 0u);
}

TEST(LockManager, UpgradeWaitsForOtherReaders) {
    LockManager lm;
    Transaction t1(1), t2(2);
    ASSERT_TRUE(lm.Lock(&t1, Row(0), LockMode::kShared));
    ASSERT_TRUE(lm.Lock(&t2, Row(0), LockMode::kShared));
    std::atomic<bool> upgraded{false};
    Joiner j;
    j.Spawn([&] {
        EXPECT_TRUE(lm.Lock(&t1, Row(0), LockMode::kExclusive));
        upgraded = true;
    });
    Pause();
    EXPECT_FALSE(upgraded);
    lm.UnlockAll(&t2);
    j.JoinAll();
    EXPECT_TRUE(upgraded);
}

TEST(LockManager, QueueIsFifoSoLaterReadersCannotStarveAWriter) {
    LockManager lm;
    Transaction t1(1), t2(2), t3(3);
    ASSERT_TRUE(lm.Lock(&t1, Row(0), LockMode::kShared));
    std::atomic<int> seq{0};
    std::atomic<int> t2_at{0}, t3_at{0};
    Joiner j;
    j.Spawn([&] {
        lm.Lock(&t2, Row(0), LockMode::kExclusive);
        t2_at = ++seq;
    });
    Pause();
    j.Spawn([&] {
        lm.Lock(&t3, Row(0), LockMode::kShared);  // compatible with t1's S, but a writer is queued ahead
        t3_at = ++seq;
    });
    Pause();
    EXPECT_EQ(t3_at.load(), 0) << "reader overtook the waiting writer";
    lm.UnlockAll(&t1);
    while (t2_at == 0) std::this_thread::sleep_for(5ms);
    Pause();
    EXPECT_EQ(t3_at.load(), 0) << "reader must wait for the writer to finish";
    lm.UnlockAll(&t2);
    j.JoinAll();
    EXPECT_EQ(t2_at.load(), 1);
    EXPECT_EQ(t3_at.load(), 2);
}

TEST(LockManager, AbortedTransactionCannotAcquireLocks) {
    LockManager lm;
    Transaction t1(1);
    t1.SetState(TxnState::kAborted);
    EXPECT_FALSE(lm.Lock(&t1, Row(0), LockMode::kShared));
}

// ---- deadlock detection ----

TEST(Deadlock, TwoTransactionsCrossLockingResolvesByAbortingTheYoungest) {
    LockManager lm;
    Transaction t1(1), t2(2);
    ASSERT_TRUE(lm.Lock(&t1, Row(0), LockMode::kExclusive));  // A
    ASSERT_TRUE(lm.Lock(&t2, Row(1), LockMode::kExclusive));  // B
    bool r1 = false, r2 = true;
    Joiner j;
    j.Spawn([&] {
        r1 = lm.Lock(&t1, Row(1), LockMode::kExclusive);  // wants B
        lm.UnlockAll(&t1);
    });
    j.Spawn([&] {
        r2 = lm.Lock(&t2, Row(0), LockMode::kExclusive);  // wants A -> cycle
        lm.UnlockAll(&t2);                                // the victim rolls back, releasing B
    });
    j.JoinAll();  // a hang here would mean the deadlock was not resolved
    EXPECT_TRUE(r1) << "older transaction survives";
    EXPECT_FALSE(r2) << "youngest transaction is the victim";
    EXPECT_EQ(t2.State(), TxnState::kAborted);
    EXPECT_EQ(t1.State(), TxnState::kActive);
    EXPECT_EQ(lm.DeadlocksDetected(), 1u);
}

TEST(Deadlock, ThreeWayCycleHasExactlyOneVictim) {
    LockManager lm;
    Transaction t1(1), t2(2), t3(3);
    Transaction* ts[3] = {&t1, &t2, &t3};
    for (int i = 0; i < 3; i++) ASSERT_TRUE(lm.Lock(ts[i], Row(i), LockMode::kExclusive));
    bool ok[3];
    Joiner j;
    for (int i = 0; i < 3; i++) {
        j.Spawn([&, i] {
            ok[i] = lm.Lock(ts[i], Row((i + 1) % 3), LockMode::kExclusive);
            lm.UnlockAll(ts[i]);
        });
    }
    j.JoinAll();
    EXPECT_TRUE(ok[0]);
    EXPECT_TRUE(ok[1]);
    EXPECT_FALSE(ok[2]);  // youngest in the cycle
    EXPECT_EQ(lm.DeadlocksDetected(), 1u);
}

TEST(Deadlock, TwoReadersUpgradingTheSameRowDeadlock) {
    LockManager lm;
    Transaction t1(1), t2(2);
    ASSERT_TRUE(lm.Lock(&t1, Row(0), LockMode::kShared));
    ASSERT_TRUE(lm.Lock(&t2, Row(0), LockMode::kShared));
    bool r1 = false, r2 = true;
    Joiner j;
    j.Spawn([&] {
        r1 = lm.Lock(&t1, Row(0), LockMode::kExclusive);
        lm.UnlockAll(&t1);
    });
    j.Spawn([&] {
        r2 = lm.Lock(&t2, Row(0), LockMode::kExclusive);
        lm.UnlockAll(&t2);
    });
    j.JoinAll();
    EXPECT_TRUE(r1);
    EXPECT_FALSE(r2);
}

TEST(Deadlock, LongWaitChainWithoutCycleAbortsNobody) {
    LockManager lm;
    Transaction t1(1), t2(2), t3(3);
    ASSERT_TRUE(lm.Lock(&t1, Row(0), LockMode::kExclusive));
    ASSERT_TRUE(lm.Lock(&t2, Row(1), LockMode::kExclusive));
    bool ok2 = false, ok3 = false;
    Joiner j;
    j.Spawn([&] {  // t2 waits for t1; t3 waits for t2: a chain, not a cycle
        ok2 = lm.Lock(&t2, Row(0), LockMode::kExclusive);
        lm.UnlockAll(&t2);
    });
    Pause();
    j.Spawn([&] {
        ok3 = lm.Lock(&t3, Row(1), LockMode::kExclusive);
        lm.UnlockAll(&t3);
    });
    Pause();
    lm.UnlockAll(&t1);
    j.JoinAll();
    EXPECT_TRUE(ok2);
    EXPECT_TRUE(ok3);
    EXPECT_EQ(lm.DeadlocksDetected(), 0u);
}

// ---- strict 2PL through the real data path (TableHeap + TransactionManager) ----

TEST(Strict2PL, NoDirtyReadsWriterCommit) {
    StressEnv env("/tmp/arbor_cc_dirty_commit", 2, 100);
    auto w = env.tm->Begin();
    env.heap->Update(w.get(), env.accounts[0], StressEnv::Fmt("acct", 0, 999));
    std::string seen;
    std::atomic<bool> read_done{false};
    Joiner j;
    j.Spawn([&] {
        auto r = env.tm->Begin();
        env.heap->Get(r.get(), env.accounts[0], &seen);  // must block on the writer's X lock
        read_done = true;
        env.tm->Commit(r.get());
    });
    Pause();
    EXPECT_FALSE(read_done) << "reader got through while the writer's change was uncommitted";
    env.tm->Commit(w.get());
    j.JoinAll();
    EXPECT_EQ(StressEnv::Value(seen), 999);
}

TEST(Strict2PL, NoDirtyReadsWriterRollbackReaderSeesOriginal) {
    StressEnv env("/tmp/arbor_cc_dirty_abort", 2, 100);
    auto w = env.tm->Begin();
    env.heap->Update(w.get(), env.accounts[0], StressEnv::Fmt("acct", 0, 999));
    std::string seen;
    Joiner j;
    j.Spawn([&] {
        auto r = env.tm->Begin();
        env.heap->Get(r.get(), env.accounts[0], &seen);
        env.tm->Commit(r.get());
    });
    Pause();
    env.tm->Abort(w.get());
    j.JoinAll();
    EXPECT_EQ(StressEnv::Value(seen), 100) << "the aborted write must never be observable";
}

TEST(Strict2PL, LocksAreHeldUntilCommitNotJustUntilTheOperationEnds) {
    StressEnv env("/tmp/arbor_cc_hold", 2, 100);
    auto t = env.tm->Begin();
    std::string s;
    env.heap->Get(t.get(), env.accounts[0], &s);
    env.heap->Get(t.get(), env.accounts[1], &s, true);
    EXPECT_EQ(env.Locks().LockCount(t->Id()), 2u);
    env.tm->Commit(t.get());
    EXPECT_EQ(env.Locks().LockCount(t->Id()), 0u);
}

TEST(Strict2PL, DeadlockThroughTheHeapAbortsOneTransactionAndTheOtherCommits) {
    StressEnv env("/tmp/arbor_cc_heap_deadlock", 2, 100);
    auto t1 = env.tm->Begin(), t2 = env.tm->Begin();
    ASSERT_LT(t1->Id(), t2->Id());
    std::string s;
    ASSERT_TRUE(env.heap->Get(t1.get(), env.accounts[0], &s, true));
    ASSERT_TRUE(env.heap->Get(t2.get(), env.accounts[1], &s, true));
    env.heap->Update(t1.get(), env.accounts[0], StressEnv::Fmt("acct", 0, 1));
    env.heap->Update(t2.get(), env.accounts[1], StressEnv::Fmt("acct", 1, 2));
    std::atomic<int> aborted{0}, committed{0};
    Joiner j;
    j.Spawn([&] {
        try {
            std::string x;
            env.heap->Get(t1.get(), env.accounts[1], &x, true);
            env.tm->Commit(t1.get());
            committed++;
        } catch (const TransactionAbortedException&) {
            env.tm->Abort(t1.get());
            aborted++;
        }
    });
    j.Spawn([&] {
        try {
            std::string x;
            env.heap->Get(t2.get(), env.accounts[0], &x, true);
            env.tm->Commit(t2.get());
            committed++;
        } catch (const TransactionAbortedException&) {
            env.tm->Abort(t2.get());
            aborted++;
        }
    });
    j.JoinAll();
    EXPECT_EQ(aborted.load(), 1);
    EXPECT_EQ(committed.load(), 1);
    EXPECT_EQ(t2->State(), TxnState::kAborted);
    // The victim's write was rolled back; the survivor's write stuck.
    auto r = env.tm->Begin();
    std::string a0, a1;
    env.heap->Get(r.get(), env.accounts[0], &a0);
    env.heap->Get(r.get(), env.accounts[1], &a1);
    env.tm->Commit(r.get());
    EXPECT_EQ(StressEnv::Value(a0), 1);
    EXPECT_EQ(StressEnv::Value(a1), 100);
}

// ---- isolation under load ----

TEST(Isolation, MixedWorkloadPreservesBankInvariantAndLosesNoUpdates) {
    StressEnv env("/tmp/arbor_cc_stress", 10, 1000);
    StressStats st;
    StressResult r = RunMixedStress(env, st, /*threads=*/8, /*total_ops=*/800);
    EXPECT_TRUE(r.ok) << r.report;
    EXPECT_EQ(st.audit_violations.load(), 0) << "an auditor observed a dirty / inconsistent state";
    EXPECT_GT(st.audits.load(), 0);
    EXPECT_EQ(env.tm->ActiveCount(), 0u);
}
