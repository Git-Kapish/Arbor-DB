# ArborDB benchmarks

Per-layer microbenchmarks, in the spirit of TECH_STACK.md: one number per layer says *which layer* does what, which an
aggregate number would hide. The suite is `bench/bench.cpp` (executable `arbordb_bench`).

```bash
# Linux / WSL
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release -j
./build-release/bin/arbordb_bench                 # everything (~3 min), medians of 3 runs
./build-release/bin/arbordb_bench --quick txn     # smoke run of one section: buffer | btree | heap | txn | sql
./build-release/bin/arbordb_bench --dir /dev/shm txn sql   # scratch dir on tmpfs: fsync is ~free

# Windows with Docker Desktop (what produced the numbers below)
powershell scripts/docker-test.ps1 -Type Release -Sanitizer none -Cmd "./bin/arbordb_bench"
powershell scripts/docker-test.ps1 -Type Release -Sanitizer none -DockerArgs "--shm-size=3g" -Cmd "./bin/arbordb_bench --dir /dev/shm txn sql"
```

## How to read these numbers (please do)

- **One machine, one run.** Docker Desktop on a Windows laptop (16 hardware threads), GCC 11 `-O3`. This is a
  characterisation of the design, not a leaderboard. Medians of 3 for the short single-threaded benchmarks; a single
  fixed-duration run (1.2 s per cell) for the multi-threaded transaction table, so expect roughly ±15% noise there.
- **fsync dominates anything that commits, and its cost moves between runs.** The suite measures it at start-up. On this
  machine it ranged from 1.4 ms to 9.4 ms across runs (Docker Desktop's virtual disk), which changes every
  fsync-bound number by up to 7x. Compare rows *within* one table, never across tables from different runs. The
  tmpfs tables below (fsync ≈ 0) isolate the CPU cost of the engine itself.
- The benchmark binary prints a warning banner instead of numbers' worth of trust if it was not built with `-O3`/NDEBUG.
  Debug/ASan/TSan builds are 5-30x slower and say nothing about the design.

## Buffer pool: LRU vs LRU-K

Pool = 200 frames over a 2000-page file; the same seeded page-id stream is replayed against each policy.

| access pattern | LRU hit rate | LRU-K hit rate | LRU fetch/s | LRU-K fetch/s |
|---|---:|---:|---:|---:|
| uniform random | 10.1% | 10.0% | 1.03M | 496.8K |
| hot set (80% of accesses to 100 pages) | 79.8% | 81.0% | 2.40M | 1.80M |
| hot set (150 pages) + repeated 400-page scans | 45.3% | **60.0%** | 1.21M | 988.2K |
| loop over 300 pages | 0.0% | 0.0% | 1.57M | 463.8K |

- LRU-K is worth it for exactly the case it was chosen for: a hot working set interrupted by sequential scans (+14.7
  points of hit rate). Everywhere else it ties; it is not a free lunch, and it does nothing for a cyclic scan larger than
  the pool (0% for both: that needs a different policy).
- **Cost:** LRU-K fetches run at roughly 30-80% of LRU's rate depending on the pattern. Reading the code, the cause is `LRUKReplacer::Victim`, which
  walks every frame (O(pool size)) on each eviction; I have not profiled it. Fine at 200 frames, it would not be at tens of
  thousands. A production LRU-K keeps evictable frames in an ordered structure (see "what I'd do at scale").

## B+Tree (INTEGER keys, 200,000 keys, page-sized nodes, 4096-frame pool)

| operation | ops/s | notes |
|---|---:|---|
| insert, ascending keys | 727.6K | |
| insert, random keys | 592.8K | tree height 3 |
| point lookup (random key) | 591.3K | all pages cached |
| full range scan (iterator) | 176.4M entries/s | leaf chain, one leaf copied at a time |
| range query: LowerBound + 100 entries | 437.7K queries/s | |
| point lookup, 64-frame pool | 376.4K | pool hit rate 84.3% (misses are served by the OS page cache) |
| delete (random order, with merges) | 611.7K | tree shrinks to empty |

Single-threaded; every operation takes one coarse tree mutex (a documented simplification), so this table says nothing
about multi-threaded index scaling.

## Heap storage and the write-ahead log (fsync ≈ 9.4 ms in this run)

| operation | tuples/s | notes |
|---|---:|---|
| insert (raw: no log, no locks) | 4.56M | 5406 pages |
| sequential scan (raw) | 6.18M | |
| point read by RID (raw) | 1.12M | |
| insert with WAL, commit every 1 | 89.4 | 89.4 durable commits/s ≈ 1 / fsync latency |
| insert with WAL, commit every 100 | 8.7K | 87.1 durable commits/s |
| insert with WAL, commit every 10000 | 270.9K | 27.1 durable commits/s |

The storage layer alone does millions of tuples/s; adding the write-ahead log with one commit per row makes the engine
exactly as fast as the disk can fsync. Batching commits is the whole game for write throughput, which is why
`LogManager` shares one fsync between concurrent committers.

## Concurrency control: strict 2PL vs MVCC (transactions of 4 point operations, 10,000-row table)

Committed transactions/s and the share of attempts that aborted (deadlock victim or write-write conflict; aborted
transactions are retried as new ones). Read-only transactions skip the commit fsync.

**fsync-bound (this run: fsync ≈ 9.4 ms)** , the realistic durable setting:

| workload | threads | 2PL txn/s | 2PL aborts | MVCC txn/s | MVCC aborts |
|---|---:|---:|---:|---:|---:|
| read-mostly (95% reads) | 1 | 429.6 | 0.0% | 538.7 | 0.0% |
| read-mostly (95% reads) | 4 | 1.2K | 0.0% | 1.2K | 0.0% |
| read-mostly (95% reads) | 8 | 2.2K | 0.0% | 2.3K | 0.0% |
| balanced (50% reads) | 1 | 130.2 | 0.0% | 128.4 | 0.0% |
| balanced (50% reads) | 4 | 214.3 | 0.0% | 150.6 | 0.0% |
| balanced (50% reads) | 8 | 484.5 | 0.0% | 517.8 | 0.2% |
| hot spot (16 rows, 50% reads) | 1 | 113.0 | 0.0% | 115.5 | 0.0% |
| hot spot (16 rows, 50% reads) | 4 | 127.7 | 4.7% | 180.1 | 33.0% |
| hot spot (16 rows, 50% reads) | 8 | 122.1 | 17.8% | 231.0 | 48.7% |

**tmpfs (fsync ≈ 0)**, which exposes the CPU/contention cost of the engine itself:

| workload | threads | 2PL txn/s | 2PL aborts | MVCC txn/s | MVCC aborts |
|---|---:|---:|---:|---:|---:|
| read-mostly (95% reads) | 1 | 460.6K | 0.0% | 656.2K | 0.0% |
| read-mostly (95% reads) | 4 | 96.5K | 0.0% | 133.6K | 0.0% |
| read-mostly (95% reads) | 8 | 69.0K | 0.0% | 96.6K | 0.0% |
| balanced (50% reads) | 1 | 248.0K | 0.0% | 221.4K | 0.0% |
| balanced (50% reads) | 4 | 51.8K | 0.0% | 51.2K | 0.1% |
| balanced (50% reads) | 8 | 38.2K | 0.0% | 42.3K | 0.1% |
| hot spot (16 rows, 50% reads) | 1 | 309.5K | 0.0% | 314.8K | 0.0% |
| hot spot (16 rows, 50% reads) | 4 | 22.9K | 8.0% | 25.4K | 27.4% |
| hot spot (16 rows, 50% reads) | 8 | 8.1K | 23.6% | 14.8K | 41.9% |

What this shows:

1. **MVCC is not faster on every workload.** With fsync-bound commits the two strategies are within noise on the
   uncontended workloads. MVCC pulls ahead under contention (hot spot, 8 threads: 231 vs 122 txn/s) because readers
   never wait for writers, but it pays for that with a much higher abort rate (48.7% vs 17.8%): first-committer-wins aborts
   losers that 2PL would have made wait. It also does slightly more work per transaction (version bookkeeping).
2. **The engine does not scale across threads once the disk is out of the way.** On tmpfs, throughput *falls* as threads
   are added (read-mostly: 460K txn/s at 1 thread, 96K at 4, 69K at 8). The single mutexes in the buffer pool,
   the lock table and the log are the likely cause (every page fetch, lock request and log append goes through one);
   I have not profiled it, so treat the attribution as a hypothesis. This is the honest ceiling of the design as built and is
   exactly the "latch crabbing, sharded pool, partitioned lock table" list in the README's "what I'd do at scale".
   It is invisible in the fsync-bound table because the disk is the bottleneck there.

### A bug the benchmark found: the log mutex was held across fsync

The first version of `LogManager::FlushTo` held the log mutex while calling `fsync`, so *every* thread, including ones that
only wanted to append a record, stalled for the whole fsync. The first benchmark run showed it as throughput that did not
move with thread count (2PL read-mostly: 4.3K txn/s at 1, 4 and 8 threads). The fix releases the mutex during
`fsync` (one leader fsyncs; followers whose records it covers just wait): classic group commit.

| workload | threads | 2PL before | 2PL after | MVCC before | MVCC after |
|---|---:|---:|---:|---:|---:|
| read-mostly | 1 | 4.3K | 3.2K | 4.2K | 2.9K |
| read-mostly | 4 | 4.3K | **8.9K** | 4.8K | **7.6K** |
| read-mostly | 8 | 4.3K | **13.7K** | 3.2K | **12.0K** |
| balanced | 1 | 751 | 597 | 807 | 668 |
| balanced | 4 | 594 | **1.6K** | 693 | **991** |
| balanced | 8 | 700 | **1.7K** | 701 | **1.4K** |

(Before: fsync ≈ 2.0 ms; after: ≈ 1.4 ms, so the single-thread rows are not comparable, the scaling shape is the
point. Both runs are the same binary otherwise.) The full test suite, the `kill -9` recovery script and the ThreadSanitizer run
all pass on the fixed log manager.

## SQL layer, end to end (20,000-row table, tmpfs, queries/s)

Parse + bind + optimize + execute through `Session::Execute`, in each concurrency mode.

| statement | 2PL queries/s | MVCC queries/s | 2PL plan | MVCC plan |
|---|---:|---:|---|---|
| bulk load (rows/s, 200-row INSERTs, one txn) | 191.1K | 246.7K | Insert | Insert |
| point select by primary key | **187.6K** | 181.3 | IndexScan | SeqScan + Filter |
| 100-row range on primary key | **14.7K** | 172.4 | IndexScan | SeqScan + Filter |
| equality on unindexed column | 76.8 | **190.2** | SeqScan + Filter | SeqScan + Filter |
| COUNT(*) over the table | 82.5 | **199.0** | SeqScan + Aggregation | SeqScan + Aggregation |
| GROUP BY (100 groups) with SUM/AVG | 73.4 | **137.9** | SeqScan + Aggregation | SeqScan + Aggregation |
| hash join t x g + GROUP BY | 61.4 | **108.6** | HashJoin | HashJoin |
| ORDER BY v LIMIT 10 | 49.7 | **89.4** | SeqScan + Sort | SeqScan + Sort |
| UPDATE by primary key (autocommit) | **141.8K** | 179.0 | IndexScan | SeqScan |
| INSERT one row (autocommit) | 150.4K | 150.1K | | |

- The B+Tree is the difference between 188K and 181 point lookups per second (1000x) on 20,000 rows. In MVCC mode
  the optimizer cannot use indexes (their entries are not versioned), so MVCC mode today suits scan-heavy workloads and
  does badly at point lookups: a real cost of the simplified design, not just a footnote.
- Under 2PL, a sequential scan takes and later releases one S lock per row it examines, which is why MVCC scans are
  about 2.5x faster (82 vs 199 COUNT(*)/s). Even so, a full scan costs 5-20 ms for 20,000 rows (1-4M rows/s): expression
  evaluation looks columns up by name per row and tuples are decoded eagerly. Correctness first; that is the obvious place to
  optimize next.
- `UPDATE`/`INSERT` autocommit rows on tmpfs measure the engine without the fsync; on a real disk they would be
  ≈ 1 / fsync latency (≈ 90/s at 9.4 ms) like the `commit every 1` heap row above.

## Reproducing and extending

Every workload is seeded; a different `--dir` or machine changes the absolute numbers but should preserve the shapes
above (LRU-K only winning on scan pollution; fsync-bound commit rates; 2PL scans slower than MVCC scans; MVCC aborting more
under hot-spot contention). The CI workflow runs `arbordb_bench --quick` in a Release build as a smoke test that the
suite still builds and runs; it does not assert on timings, because shared CI runners are far too noisy for that.
