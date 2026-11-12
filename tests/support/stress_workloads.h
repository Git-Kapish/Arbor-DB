#pragma once
// Concurrent workloads that prove isolation properties of strict 2PL. Shared by tests/test_concurrency.cpp
// and tools/concurrency_stress_test.cpp.
//
// Data: N account rows (balances) + one counter row, all fixed-length tuples in one TableHeap.
//   * lost updates : many threads do read-then-write increments on the counter; the final value must equal the
//                    number of committed increments (S->X upgrades race constantly, deadlocks get resolved).
//   * dirty reads  : transfers write the debit, yield, then write the credit (and sometimes roll back instead);
//                    auditors sum all accounts in one transaction -- any uncommitted half-transfer would show up as
//                    a wrong total. Because locks are held to commit, the total must ALWAYS equal the initial sum.
//   * isolation    : the bank invariant + repeated-read check below.
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "concurrency/lock_manager.h"
#include "concurrency/transaction_manager.h"
#include "storage/table/table_heap.h"

namespace arbordb {

struct StressEnv {
    std::string dir;
    std::unique_ptr<DiskManager> dm;
    std::unique_ptr<LogManager> log;
    std::unique_ptr<BufferPoolManager> bpm;
    TwoPLManager cc;
    std::unique_ptr<TransactionManager> tm;
    std::unique_ptr<TableHeap> heap;
    std::vector<RID> accounts;
    RID counter;
    int64_t initial_balance;

    static std::string Fmt(const char* tag, int id, int64_t v) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s%04d:%016lld", tag, id, static_cast<long long>(v));
        return buf;
    }
    static int64_t Value(const std::string& s) { return std::atoll(s.c_str() + s.find(':') + 1); }

    StressEnv(const std::string& d, int n_accounts, int64_t initial) : dir(d), initial_balance(initial) {
        std::string cmd = "rm -rf " + dir + " && mkdir -p " + dir;
        if (system(cmd.c_str()) != 0) throw std::runtime_error("mkdir failed");
        dm = std::make_unique<DiskManager>(dir + "/data.db");
        log = std::make_unique<LogManager>(dir + "/wal.log");
        bpm = std::make_unique<BufferPoolManager>(256, dm.get(), log.get());
        tm = std::make_unique<TransactionManager>(log.get(), bpm.get(), &cc);
        heap = std::make_unique<TableHeap>(bpm.get(), log.get(), &cc, TableHeap::CreateFirstPage(bpm.get()));
        auto t = tm->Begin();
        for (int i = 0; i < n_accounts; i++) accounts.push_back(heap->Insert(t.get(), Fmt("acct", i, initial)));
        counter = heap->Insert(t.get(), Fmt("ctr_", 0, 0));
        tm->Commit(t.get());
    }
    int64_t ExpectedTotal() const { return initial_balance * static_cast<int64_t>(accounts.size()); }
};

struct StressStats {
    std::atomic<long> transfers_committed{0}, transfers_rolled_back{0}, deadlock_aborts{0};
    std::atomic<long> audits{0}, audit_violations{0};
    std::atomic<long> increments_committed{0};
};

// Run `fn` until it returns true; a deadlock-victim exception rolls the transaction back and retries.
template <typename Fn>
bool RunWithRetry(StressEnv& env, StressStats& st, Fn fn) {
    while (true) {
        auto txn = env.tm->Begin();
        try {
            bool committed = fn(txn.get());
            if (committed) env.tm->Commit(txn.get());
            else env.tm->Abort(txn.get());
            return committed;
        } catch (const TransactionAbortedException&) {
            env.tm->Abort(txn.get());
            st.deadlock_aborts++;
            std::this_thread::yield();
        }
    }
}

// Move money between two random accounts, locking them in random order (so deadlocks really happen).
inline void TransferOnce(StressEnv& env, StressStats& st, std::mt19937& rng) {
    int a = rng() % env.accounts.size(), b = rng() % env.accounts.size();
    if (a == b) b = (b + 1) % env.accounts.size();
    int64_t amt = 1 + rng() % 50;
    bool rollback = rng() % 10 == 0;
    bool done = RunWithRetry(env, st, [&](Transaction* txn) {
        std::string sa, sb;
        if (!env.heap->Get(txn, env.accounts[a], &sa, true)) throw std::logic_error("account vanished");
        if (!env.heap->Get(txn, env.accounts[b], &sb, true)) throw std::logic_error("account vanished");
        env.heap->Update(txn, env.accounts[a], StressEnv::Fmt("acct", a, StressEnv::Value(sa) - amt));
        std::this_thread::yield();  // widen the window in which a dirty read would be visible
        env.heap->Update(txn, env.accounts[b], StressEnv::Fmt("acct", b, StressEnv::Value(sb) + amt));
        return !rollback;
    });
    (done ? st.transfers_committed : st.transfers_rolled_back)++;
}

// Sum every account in one transaction (scan takes S locks held until commit).
inline void AuditOnce(StressEnv& env, StressStats& st) {
    int64_t total = 0;
    RunWithRetry(env, st, [&](Transaction* txn) {
        total = 0;
        std::string first, again;
        env.heap->Get(txn, env.accounts[0], &first);
        TableHeap::Cursor c = env.heap->Begin();
        RID rid;
        std::string row;
        while (env.heap->Next(txn, &c, &rid, &row))
            if (row.compare(0, 4, "acct") == 0) total += StressEnv::Value(row);
        // Repeatable read: the S lock taken above is still held, so the row cannot have changed.
        env.heap->Get(txn, env.accounts[0], &again);
        if (again != first) st.audit_violations++;
        return true;
    });
    st.audits++;
    if (total != env.ExpectedTotal()) st.audit_violations++;
}

// Read-then-write increment: the textbook lost-update race. Read takes S, write upgrades to X.
inline void IncrementOnce(StressEnv& env, StressStats& st) {
    RunWithRetry(env, st, [&](Transaction* txn) {
        std::string s;
        env.heap->Get(txn, env.counter, &s, false);
        int64_t v = StressEnv::Value(s);
        std::this_thread::yield();
        env.heap->Update(txn, env.counter, StressEnv::Fmt("ctr_", 0, v + 1));
        return true;
    });
    st.increments_committed++;
}

struct StressResult {
    bool ok{true};
    std::string report;
};

// `threads` concurrent workers share `total_ops` transactions: ~80% transfers, ~10% audits, ~10% increments.
inline StressResult RunMixedStress(StressEnv& env, StressStats& st, int threads, long total_ops) {
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; t++) {
        ts.emplace_back([&, t] {
            std::mt19937 rng(1000 + t);  // seeded per thread
            long mine = total_ops / threads + (t < total_ops % threads ? 1 : 0);
            for (long i = 0; i < mine; i++) {
                int dice = rng() % 10;
                if (dice < 8) TransferOnce(env, st, rng);
                else if (dice < 9) AuditOnce(env, st);
                else IncrementOnce(env, st);
            }
        });
    }
    for (auto& t : ts) t.join();

    StressResult r;
    auto txn = env.tm->Begin();
    int64_t total = 0, ctr = 0;
    TableHeap::Cursor c = env.heap->Begin();
    RID rid;
    std::string row;
    while (env.heap->Next(txn.get(), &c, &rid, &row)) {
        if (row.compare(0, 4, "acct") == 0) total += StressEnv::Value(row);
        else ctr = StressEnv::Value(row);
    }
    env.tm->Commit(txn.get());
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "transfers committed=%ld rolled back=%ld | audits=%ld violations=%ld | increments=%ld counter=%lld | "
                  "deadlock aborts=%ld | final total=%lld expected=%lld",
                  st.transfers_committed.load(), st.transfers_rolled_back.load(), st.audits.load(),
                  st.audit_violations.load(), st.increments_committed.load(), static_cast<long long>(ctr),
                  st.deadlock_aborts.load(), static_cast<long long>(total), static_cast<long long>(env.ExpectedTotal()));
    r.report = buf;
    r.ok = st.audit_violations == 0 && total == env.ExpectedTotal() && ctr == st.increments_committed.load();
    return r;
}

}  // namespace arbordb
