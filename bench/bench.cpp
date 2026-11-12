// arbordb_bench: per-layer microbenchmarks (see docs/BENCHMARKS.md for results and how to read them).
//
//   arbordb_bench [--quick] [--repeat N] [--dir PATH] [buffer] [btree] [heap] [txn] [sql]
//
// Each section targets ONE layer, on purpose (TECH_STACK.md: per-layer numbers explain which layer does what;
// one end-to-end number hides it). Run it from a Release build: Debug/sanitizer builds are 5-30x slower and
// say nothing about the design. Workloads are seeded and deterministic; timings are medians of --repeat runs
// where a run is short (buffer pool, B+Tree, heap, SQL), and a single fixed-duration run for the multi-threaded
// transaction section. fsync-bound numbers depend entirely on the storage under --dir; the header prints the
// measured fsync latency so they can be interpreted.
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "buffer/lru_k_replacer.h"
#include "buffer/lru_replacer.h"
#include "concurrency/lock_manager.h"
#include "concurrency/mvcc_manager.h"
#include "concurrency/transaction_manager.h"
#include "db/database.h"
#include "index/bplus_tree.h"
#include "storage/table/table_heap.h"

using namespace arbordb;
using Clock = std::chrono::steady_clock;

namespace {
struct Options {
    bool quick = false;
    int repeat = 3;
    std::string dir = "/tmp";
} opt;
std::string g_root;  // scratch directory for this run

double Seconds(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

double Median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
// Runs fn (returns a throughput) `repeat` times and reports the median.
double Med(const std::function<double()>& fn) {
    std::vector<double> r;
    for (int i = 0; i < opt.repeat; i++) r.push_back(fn());
    return Median(r);
}
std::string Fmt(double v) {
    char b[32];
    if (v >= 1e6) std::snprintf(b, sizeof b, "%.2fM", v / 1e6);
    else if (v >= 1e3) std::snprintf(b, sizeof b, "%.1fK", v / 1e3);
    else std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}
std::string Pct(double v) {
    char b[16];
    std::snprintf(b, sizeof b, "%.1f%%", v * 100);
    return b;
}
std::string Path(const std::string& name) {
    std::string p = g_root + "/" + name;
    unlink(p.c_str());
    unlink((p + ".master").c_str());
    return p;
}
void Row(const std::vector<std::string>& cells) {
    std::printf("|");
    for (auto& c : cells) std::printf(" %s |", c.c_str());
    std::printf("\n");
}
void Header(const std::vector<std::string>& cols) {
    Row(cols);
    std::printf("|");
    for (auto& c : cols) {
        bool text = c == "notes" || c == "plan" || c == "2PL plan" || c == "MVCC plan" || c == "operation" || c == "statement" || c == "workload" || c == "access pattern";
        std::printf("%s|", text ? "---" : "---:");
    }
    std::printf("\n");
}
void Section(const std::string& title, const std::string& note) { std::printf("\n### %s\n\n%s\n\n", title.c_str(), note.c_str()); }

// ---------------------------------------------------------------------------------------------- environment
double MeasureFsyncMs() {
    int fd = open(Path("fsync_probe").c_str(), O_RDWR | O_CREAT, 0644);
    char buf[4096] = {};
    const int n = 50;
    auto t0 = Clock::now();
    for (int i = 0; i < n; i++) {
        pwrite(fd, buf, sizeof buf, static_cast<off_t>(i) * 4096);
        fsync(fd);
    }
    double ms = Seconds(t0, Clock::now()) * 1000 / n;
    close(fd);
    return ms;
}

// ---------------------------------------------------------------------------------------------- buffer pool
// Hit rate of the same access stream under plain LRU and LRU-K. The pool holds 200 of 2000 pages.
std::vector<page_id_t> MakePattern(const std::string& kind, std::size_t n, std::mt19937_64& rng, page_id_t first) {
    std::vector<page_id_t> seq;
    seq.reserve(n);
    const int kPages = 2000, kHot = 100;
    if (kind == "uniform random") {
        for (std::size_t i = 0; i < n; i++) seq.push_back(first + static_cast<page_id_t>(rng() % kPages));
    } else if (kind == "hot set (80% of accesses to 100 pages)") {
        for (std::size_t i = 0; i < n; i++) {
            bool hot = rng() % 100 < 80;
            seq.push_back(first + static_cast<page_id_t>(hot ? rng() % kHot : kHot + rng() % (kPages - kHot)));
        }
    } else if (kind == "hot set (150 pages) + repeated 400-page scans") {  // the case LRU-K exists for
        // 600 random hot accesses, then a sequential pass over the next 400 cold pages (never reused soon),
        // repeat. Scan misses are unavoidable; what differs is whether the scan also evicts the hot set.
        int cold = 150;
        while (seq.size() < n) {
            for (int i = 0; i < 600 && seq.size() < n; i++) seq.push_back(first + static_cast<page_id_t>(rng() % 150));
            for (int i = 0; i < 400 && seq.size() < n; i++) {
                seq.push_back(first + cold);
                cold = cold + 1 < kPages ? cold + 1 : 150;
            }
        }
    } else {  // "loop over 300 pages": cyclic scan slightly larger than the pool; LRU's worst case
        for (std::size_t i = 0; i < n; i++) seq.push_back(first + static_cast<page_id_t>(i % 300));
    }
    return seq;
}

void BenchBuffer() {
    Section("Buffer pool: eviction policy vs access pattern",
            "Pool = 200 frames over a 2000-page file. Same page-id stream replayed against each policy; "
            "hit rate comes from `BufferPoolManager::Stats()`. LRU-K uses K=2.");
    DiskManager dm(Path("bench_buffer.db"));
    page_id_t first = dm.AllocatePage();
    for (int i = 1; i < 2000; i++) dm.AllocatePage();
    char z[kPageSize] = {};
    for (int i = 0; i < 2000; i++) dm.WritePage(first + i, z);

    const std::size_t n = opt.quick ? 100000 : 400000;
    Header({"access pattern", "LRU hit rate", "LRU-K hit rate", "LRU fetch/s", "LRU-K fetch/s"});
    for (std::string kind : {"uniform random", "hot set (80% of accesses to 100 pages)", "hot set (150 pages) + repeated 400-page scans",
                             "loop over 300 pages"}) {
        std::mt19937_64 rng(42);
        auto seq = MakePattern(kind, n, rng, first);
        double rate[2], tput[2];
        for (int policy = 0; policy < 2; policy++) {
            std::vector<double> rs, ts;
            for (int rep = 0; rep < opt.repeat; rep++) {
                std::unique_ptr<Replacer> r;
                if (policy == 0) r = std::make_unique<LRUReplacer>();
                else r = std::make_unique<LRUKReplacer>(2);
                BufferPoolManager bpm(200, &dm, nullptr, std::move(r));
                for (std::size_t i = 0; i < 2000 && i < seq.size(); i++) {}  // (pool starts cold)
                auto t0 = Clock::now();
                for (page_id_t id : seq) {
                    Page* p = bpm.FetchPage(id);
                    bpm.UnpinPage(p->GetPageId(), false);
                }
                ts.push_back(static_cast<double>(seq.size()) / Seconds(t0, Clock::now()));
                rs.push_back(bpm.Stats().HitRate());
            }
            rate[policy] = Median(rs);
            tput[policy] = Median(ts);
        }
        Row({kind, Pct(rate[0]), Pct(rate[1]), Fmt(tput[0]), Fmt(tput[1])});
    }
}

// ---------------------------------------------------------------------------------------------- B+Tree
void BenchBTree() {
    const int64_t n = opt.quick ? 50000 : 200000;
    Section("B+Tree index (INTEGER keys, page-sized nodes)",
            "Keys " + std::to_string(n) + ", 4096-frame pool unless noted. Single thread, one coarse tree latch.");
    std::vector<int64_t> keys(n);
    for (int64_t i = 0; i < n; i++) keys[i] = i;
    std::mt19937_64 rng(7);
    std::vector<int64_t> shuffled = keys;
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
    auto rid = [](int64_t k) { return RID{static_cast<page_id_t>(k), 0}; };

    Header({"operation", "ops/s", "notes"});
    auto fresh = [&](std::size_t pool, const char* name, std::unique_ptr<DiskManager>* dm, std::unique_ptr<BufferPoolManager>* bpm) {
        *dm = std::make_unique<DiskManager>(Path(name));
        *bpm = std::make_unique<BufferPoolManager>(pool, dm->get());
    };
    std::unique_ptr<DiskManager> dm;
    std::unique_ptr<BufferPoolManager> bpm;

    Row({"insert, ascending keys", Fmt(Med([&] {
             fresh(4096, "bt1.db", &dm, &bpm);
             BPlusTree<int64_t> t(bpm.get());
             auto t0 = Clock::now();
             for (int64_t k : keys) t.Insert(k, rid(k));
             return static_cast<double>(n) / Seconds(t0, Clock::now());
         })), "each split leaves the left leaf half full"});
    int height = 0;
    Row({"insert, random keys", Fmt(Med([&] {
             fresh(4096, "bt2.db", &dm, &bpm);
             BPlusTree<int64_t> t(bpm.get());
             auto t0 = Clock::now();
             for (int64_t k : shuffled) t.Insert(k, rid(k));
             double r = static_cast<double>(n) / Seconds(t0, Clock::now());
             height = t.Height();
             return r;
         })), "tree height " + std::to_string(height)});

    // One tree reused for read benchmarks.
    fresh(4096, "bt3.db", &dm, &bpm);
    BPlusTree<int64_t> tree(bpm.get());
    for (int64_t k : shuffled) tree.Insert(k, rid(k));
    std::vector<int64_t> probe = shuffled;
    std::shuffle(probe.begin(), probe.end(), rng);
    Row({"point lookup (random key)", Fmt(Med([&] {
             RID r;
             auto t0 = Clock::now();
             for (int64_t k : probe) tree.GetValue(k, &r);
             return static_cast<double>(n) / Seconds(t0, Clock::now());
         })), "all pages cached"});
    Row({"full range scan (iterator)", Fmt(Med([&] {
             auto t0 = Clock::now();
             std::size_t c = 0;
             for (auto it = tree.Begin(); it.Valid(); it.Next()) c++;
             return static_cast<double>(c) / Seconds(t0, Clock::now());
         })), "entries/s via the leaf chain"});
    Row({"range query: LowerBound + 100 entries", Fmt(Med([&] {
             const int q = opt.quick ? 5000 : 20000;
             auto t0 = Clock::now();
             for (int i = 0; i < q; i++) {
                 auto it = tree.LowerBound(static_cast<int64_t>(rng() % (n - 200)));
                 for (int j = 0; j < 100 && it.Valid(); j++) it.Next();
             }
             return static_cast<double>(q) / Seconds(t0, Clock::now());
         })), "queries/s"});

    // Same lookups with a pool far smaller than the tree: shows the buffer pool, not the tree, becoming the limit.
    {
        fresh(64, "bt4.db", &dm, &bpm);
        BPlusTree<int64_t> small(bpm.get());
        for (int64_t k : shuffled) small.Insert(k, rid(k));
        bpm->ResetStats();
        RID r;
        auto t0 = Clock::now();
        for (int64_t k : probe) small.GetValue(k, &r);
        double ops = static_cast<double>(n) / Seconds(t0, Clock::now());
        Row({"point lookup, 64-frame pool", Fmt(ops), "pool hit rate " + Pct(bpm->Stats().HitRate()) + " (misses read the OS page cache)"});
    }

    Row({"delete (random order, with merges)", Fmt(Med([&] {
             fresh(4096, "bt5.db", &dm, &bpm);
             BPlusTree<int64_t> t(bpm.get());
             for (int64_t k : shuffled) t.Insert(k, rid(k));
             auto t0 = Clock::now();
             for (int64_t k : probe) t.Remove(k);
             return static_cast<double>(n) / Seconds(t0, Clock::now());
         })), "tree shrinks to empty"});
}

// ---------------------------------------------------------------------------------------------- heap + WAL
void BenchHeap() {
    const int n = opt.quick ? 50000 : 200000;
    Section("Heap storage and the write-ahead log",
            "100-byte tuples in slotted pages. The 'raw' rows bypass logging and locking (storage layer alone); the "
            "WAL rows add transactions, logging and a commit per N inserts. One fsync = one durable commit.");
    std::string tuple(100, 'x');
    Header({"operation", "tuples/s", "notes"});

    {
        // Every repeat starts from an empty table; the pool (32 MB) holds the whole table, so this is the
        // storage layer, not the disk.
        std::unique_ptr<DiskManager> dmp;
        std::unique_ptr<BufferPoolManager> bpmp;
        std::unique_ptr<TableHeap> heapp;
        std::vector<RID> rids;
        double ins = Med([&] {
            heapp.reset();
            bpmp.reset();
            dmp = std::make_unique<DiskManager>(Path("heap_raw.db"));
            bpmp = std::make_unique<BufferPoolManager>(8192, dmp.get());
            heapp = std::make_unique<TableHeap>(bpmp.get(), nullptr, nullptr, TableHeap::CreateFirstPage(bpmp.get()));
            rids.clear();
            auto t0 = Clock::now();
            for (int i = 0; i < n; i++) rids.push_back(heapp->Insert(nullptr, tuple));
            return static_cast<double>(n) / Seconds(t0, Clock::now());
        });
        TableHeap& heap = *heapp;
        Row({"insert (raw)", Fmt(ins), std::to_string(heap.NumPages()) + " pages"});
        Row({"sequential scan (raw)", Fmt(Med([&] {
                 auto t0 = Clock::now();
                 TableHeap::Cursor c = heap.Begin();
                 RID r;
                 std::string out;
                 std::size_t cnt = 0;
                 while (heap.Next(nullptr, &c, &r, &out)) cnt++;
                 return static_cast<double>(cnt) / Seconds(t0, Clock::now());
             })), "slot walk + page latch per slot"});
        std::mt19937_64 rng(3);
        Row({"point read by RID (raw)", Fmt(Med([&] {
                 std::string out;
                 auto t0 = Clock::now();
                 for (int i = 0; i < n; i++) heap.Get(nullptr, rids[rng() % rids.size()], &out);
                 return static_cast<double>(n) / Seconds(t0, Clock::now());
             })), ""});
    }

    for (int batch : {1, 100, 10000}) {
        DiskManager dm(Path("heap_wal.db"));
        LogManager log(Path("heap_wal.log"));
        BufferPoolManager bpm(4096, &dm, &log);
        TransactionManager tm(&log, &bpm, nullptr);
        TableHeap heap(&bpm, &log, nullptr, TableHeap::CreateFirstPage(&bpm));
        int total = batch == 1 ? (opt.quick ? 300 : 1000) : n;
        auto t0 = Clock::now();
        std::unique_ptr<Transaction> txn;
        for (int i = 0; i < total; i++) {
            if (!txn) txn = tm.Begin();
            heap.Insert(txn.get(), tuple);
            if ((i + 1) % batch == 0) {
                tm.Commit(txn.get());
                txn.reset();
            }
        }
        if (txn) tm.Commit(txn.get());
        double secs = Seconds(t0, Clock::now());
        Row({"insert with WAL, commit every " + std::to_string(batch), Fmt(total / secs),
             Fmt(std::max(1, total / batch) / secs) + " durable commits/s"});
    }
}

// ---------------------------------------------------------------------------------------------- transactions
struct TxnEnv {
    std::unique_ptr<DiskManager> dm;
    std::unique_ptr<LogManager> log;
    std::unique_ptr<BufferPoolManager> bpm;
    std::unique_ptr<ConcurrencyControlManager> cc;
    std::unique_ptr<TransactionManager> tm;
    std::unique_ptr<TableHeap> heap;
    std::vector<RID> rids;

    TxnEnv(bool mvcc, int rows) {
        dm = std::make_unique<DiskManager>(Path("txn.db"));
        log = std::make_unique<LogManager>(Path("txn.log"));
        bpm = std::make_unique<BufferPoolManager>(4096, dm.get(), log.get());
        if (mvcc) cc = std::make_unique<MVCCManager>();
        else cc = std::make_unique<TwoPLManager>();
        tm = std::make_unique<TransactionManager>(log.get(), bpm.get(), cc.get());
        heap = std::make_unique<TableHeap>(bpm.get(), log.get(), cc.get(), TableHeap::CreateFirstPage(bpm.get()));
        auto t = tm->Begin();
        for (int i = 0; i < rows; i++) rids.push_back(heap->Insert(t.get(), std::string(48, 'a' + i % 26)));
        tm->Commit(t.get());
    }
};

struct TxnResult {
    double committed_per_sec;
    double abort_rate;
};

// Fixed-duration run: `threads` workers each loop { begin; 4 point operations; commit }. An operation is a
// read with probability read_pct, otherwise a read-modify-write of one row (read for update, then update).
TxnResult RunTxnWorkload(TxnEnv& env, int threads, int rows_used, int read_pct, double seconds) {
    std::atomic<bool> stop{false};
    std::atomic<long> committed{0}, aborted{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; t++) {
        ts.emplace_back([&, t] {
            std::mt19937_64 rng(100 + t);
            std::string buf;
            while (!stop) {
                auto txn = env.tm->Begin();
                try {
                    for (int op = 0; op < 4; op++) {
                        const RID& rid = env.rids[rng() % rows_used];
                        bool write = static_cast<int>(rng() % 100) >= read_pct;
                        env.heap->Get(txn.get(), rid, &buf, write);
                        if (write) {
                            buf[0] = static_cast<char>('a' + rng() % 26);
                            env.heap->Update(txn.get(), rid, buf);
                        }
                    }
                    env.tm->Commit(txn.get());
                    committed++;
                } catch (const TransactionAbortedException&) {
                    env.tm->Abort(txn.get());
                    aborted++;
                }
            }
        });
    }
    auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    stop = true;
    for (auto& t : ts) t.join();
    double secs = Seconds(t0, Clock::now());
    long total = committed + aborted;
    return {static_cast<double>(committed) / secs, total ? static_cast<double>(aborted) / static_cast<double>(total) : 0.0};
}

void BenchTxn() {
    Section("Concurrency control: strict 2PL vs MVCC (snapshot isolation)",
            "Transactions of 4 point operations on 10,000-row table (hot-spot: 16 rows). Committed transactions per "
            "second and share of attempts that aborted (deadlock victim or write-write conflict, then retried as new "
            "transactions). Read-only transactions skip the commit fsync; any transaction that writes pays one, shared "
            "between concurrent committers (group commit).");
    const double secs = opt.quick ? 0.4 : 1.2;
    struct W { const char* name; int rows, read_pct; };
    Header({"workload", "threads", "2PL txn/s", "2PL aborts", "MVCC txn/s", "MVCC aborts"});
    for (W w : {W{"read-mostly (95% reads)", 10000, 95}, W{"balanced (50% reads)", 10000, 50},
                W{"hot spot (16 rows, 50% reads)", 16, 50}}) {
        for (int threads : {1, 4, 8}) {
            TxnResult r[2];
            for (int mvcc = 0; mvcc < 2; mvcc++) {
                TxnEnv env(mvcc, 10000);
                r[mvcc] = RunTxnWorkload(env, threads, w.rows, w.read_pct, secs);
            }
            Row({w.name, std::to_string(threads), Fmt(r[0].committed_per_sec), Pct(r[0].abort_rate), Fmt(r[1].committed_per_sec),
                 Pct(r[1].abort_rate)});
        }
    }
}

// ---------------------------------------------------------------------------------------------- SQL
struct SqlCase {
    const char* label;
    const char* plan_2pl;
    const char* plan_mvcc;
};

// Runs the whole statement list against a fresh database in the given mode; returns queries/s per case
// (index 0 = bulk load in rows/s).
std::vector<double> SqlSuite(ConcurrencyMode mode, int rows, const std::vector<SqlCase>& cases) {
    std::string dir = g_root + (mode == ConcurrencyMode::kMVCC ? "/sqldb_mvcc" : "/sqldb");
    std::string cmd = "rm -rf " + dir;
    if (system(cmd.c_str()) != 0) return {};
    DatabaseOptions o;
    o.buffer_pool_pages = 4096;
    o.concurrency = mode;
    Database db(dir, o);
    Session s(&db);
    s.Execute("CREATE TABLE t (id INTEGER PRIMARY KEY, grp INTEGER, v INTEGER, s VARCHAR(40))");
    s.Execute("CREATE TABLE g (id INTEGER PRIMARY KEY, name VARCHAR(20))");

    std::vector<double> out;
    auto t0 = Clock::now();
    s.Execute("BEGIN");
    for (int base = 0; base < rows; base += 200) {
        std::string sql = "INSERT INTO t VALUES ";
        for (int i = base; i < std::min(rows, base + 200); i++)
            sql += std::string(i > base ? "," : "") + "(" + std::to_string(i) + "," + std::to_string(i % 100) + "," +
                   std::to_string((i * 7919) % 10007) + ",'row-" + std::to_string(i) + "')";
        s.Execute(sql);
    }
    std::string gsql = "INSERT INTO g VALUES ";
    for (int i = 0; i < 100; i++) gsql += std::string(i ? "," : "") + "(" + std::to_string(i) + ",'group-" + std::to_string(i) + "')";
    s.Execute(gsql);
    s.Execute("COMMIT");
    out.push_back(rows / Seconds(t0, Clock::now()));

    std::mt19937_64 rng(11);
    int next_id = 1000000;
    std::vector<std::function<std::string()>> gen = {
        [&] { return "SELECT s FROM t WHERE id = " + std::to_string(rng() % rows) + ";"; },
        [&] { int a = static_cast<int>(rng() % (rows - 100)); return "SELECT id, v FROM t WHERE id >= " + std::to_string(a) + " AND id < " + std::to_string(a + 100) + ";"; },
        [&] { return "SELECT id FROM t WHERE v = " + std::to_string(rng() % 10007) + ";"; },
        [&] { return std::string("SELECT COUNT(*) FROM t;"); },
        [&] { return std::string("SELECT grp, COUNT(*), SUM(v), AVG(v) FROM t GROUP BY grp;"); },
        [&] { return std::string("SELECT g.name, COUNT(*) FROM t JOIN g ON t.grp = g.id GROUP BY g.name;"); },
        [&] { return std::string("SELECT id, v FROM t ORDER BY v DESC LIMIT 10;"); },
        [&] { return "UPDATE t SET v = v + 1 WHERE id = " + std::to_string(rng() % rows) + ";"; },
        [&] { return "INSERT INTO t VALUES (" + std::to_string(next_id++) + ",0,0,'x');"; },
    };
    (void)cases;
    for (auto& make : gen) {
        out.push_back(Med([&] {
            int n = 0;
            auto start = Clock::now();
            while (Seconds(start, Clock::now()) < (opt.quick ? 0.15 : 0.4) || n < 3) {
                s.Execute(make());
                n++;
            }
            return n / Seconds(start, Clock::now());
        }));
    }
    return out;
}

void BenchSql() {
    const int rows = opt.quick ? 5000 : 20000;
    Section("SQL layer, end to end (parse + bind + optimize + execute)",
            "Table t(id PRIMARY KEY, grp, v, s) with " + std::to_string(rows) + " rows, table g(id PRIMARY KEY, name) with 100 rows, "
            "run through `Session::Execute` in each concurrency mode (queries/s, median of repeats). Under 2PL a scan takes an "
            "S lock per row it visits; MVCC reads take no locks but the optimizer cannot use B+Tree indexes in that mode.");
    std::vector<SqlCase> cases = {
        {"bulk load (rows/s, 200-row INSERTs, one txn)", "Insert", "Insert"},
        {"point select by primary key", "IndexScan", "SeqScan + Filter"},
        {"100-row range on primary key", "IndexScan", "SeqScan + Filter"},
        {"equality on unindexed column", "SeqScan + Filter", "SeqScan + Filter"},
        {"COUNT(*) over the table", "SeqScan + Aggregation", "SeqScan + Aggregation"},
        {"GROUP BY (100 groups) with SUM/AVG", "SeqScan + Aggregation", "SeqScan + Aggregation"},
        {"hash join t x g + GROUP BY", "HashJoin", "HashJoin"},
        {"ORDER BY v LIMIT 10", "SeqScan + Sort", "SeqScan + Sort"},
        {"UPDATE by primary key (autocommit)", "IndexScan, 1 fsync", "SeqScan, 1 fsync"},
        {"INSERT one row (autocommit)", "1 fsync", "1 fsync"},
    };
    auto a = SqlSuite(ConcurrencyMode::kTwoPL, rows, cases);
    auto b = SqlSuite(ConcurrencyMode::kMVCC, rows, cases);
    if (a.size() != cases.size() || b.size() != cases.size()) return;
    Header({"statement", "2PL queries/s", "MVCC queries/s", "2PL plan", "MVCC plan"});
    for (std::size_t i = 0; i < cases.size(); i++) Row({cases[i].label, Fmt(a[i]), Fmt(b[i]), cases[i].plan_2pl, cases[i].plan_mvcc});
}
}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> sections;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--quick") opt.quick = true, opt.repeat = 1;
        else if (a == "--repeat" && i + 1 < argc) opt.repeat = std::max(1, std::atoi(argv[++i]));
        else if (a == "--dir" && i + 1 < argc) opt.dir = argv[++i];
        else sections.push_back(a);
    }
    if (sections.empty()) sections = {"buffer", "btree", "heap", "txn", "sql"};

    std::string tmpl = opt.dir + "/arbor_bench_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) {
        std::fprintf(stderr, "cannot create scratch dir under %s\n", opt.dir.c_str());
        return 1;
    }
    g_root = buf.data();

    std::printf("## ArborDB benchmarks\n\n");
#ifdef NDEBUG
    std::printf("- build: optimized (NDEBUG)\n");
#else
    std::printf("- build: **NOT optimized** (no NDEBUG): numbers are meaningless, rebuild with -DCMAKE_BUILD_TYPE=Release\n");
#endif
    std::printf("- hardware threads: %u, page size: %zu bytes, mode: %s (repeat %d)\n", std::thread::hardware_concurrency(), kPageSize,
                opt.quick ? "quick" : "full", opt.repeat);
    std::printf("- scratch dir: %s, measured fsync latency: %.2f ms\n", opt.dir.c_str(), MeasureFsyncMs());

    for (auto& s : sections) {
        if (s == "buffer") BenchBuffer();
        else if (s == "btree") BenchBTree();
        else if (s == "heap") BenchHeap();
        else if (s == "txn") BenchTxn();
        else if (s == "sql") BenchSql();
        else std::fprintf(stderr, "unknown section '%s'\n", s.c_str());
        std::fflush(stdout);
    }
    std::string cmd = "rm -rf " + g_root;
    (void)!system(cmd.c_str());
    return 0;
}
