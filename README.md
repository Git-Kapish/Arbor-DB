# ArborDB

A single-node, disk-based relational database engine written from scratch in C++17. No embedded database, no third-party storage engine, no parser generator: every layer is hand-built.

```
SQL ─▶ Parser ─▶ Binder ─▶ Optimizer ─▶ Executors (Volcano) ─▶ TableHeap / B+Tree ─▶ Buffer pool (LRU-K) ─▶ DiskManager
         (recursive   (names,    (rule-based)   Init()/Next()        │  row locks (strict 2PL, deadlock detection)
          descent)     types)                                         └─ write-ahead log ─▶ ARIES recovery (Analysis → Redo → Undo)
```

Design docs: [PRD](PRD.md) · [ARCHITECTURE](ARCHITECTURE.md) · [TECH_STACK](TECH_STACK.md) · [AGENTS](AGENTS.md) · [implementation plan](docs/IMPLEMENTATION_PLAN.md)

## Status

| Milestone | State | What proves it |
|---|---|---|
| M1 Storage (`DiskManager`, slotted pages) | done | `test_disk_manager`, `test_slotted_page` (byte-identical across reopen, free-list reuse, page-full edge case) |
| M2 Buffer pool + LRU-K | done | `test_buffer_pool` (victim differs from plain LRU, scan resistance, pinned pages never evicted, all-pinned returns `nullptr`, WAL rule hook) |
| M3 B+Tree | done | `test_bplus_tree` (seeded random insert/delete vs `std::map`, invariants checked after **every** op for six fan-outs, forced root split, one-level-at-a-time shrink) |
| M4 WAL + ARIES recovery | done | `test_recovery` + `scripts/crash_recovery_test.sh` (real `kill -9`) |
| M5 Strict 2PL + deadlock detection | done | `test_concurrency` + `concurrency_stress_test` |
| M6 SQL layer | done | `test_query_layer` (hand-computed fixtures in `tests/sql/`), shell `arbordb` |
| M7 TCP server + client | done | `test_net` |
| M7 MVCC | **not implemented** (stretch, see Known Gaps) | — |

## Build and run

The target platform is Linux (POSIX `pread`/`pwrite`/`fsync`, real `kill -9`). On Windows use the Docker dev image:

```bash
# Linux / WSL
sudo apt install -y build-essential cmake libgtest-dev
mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) && ctest --output-on-failure

# Windows with Docker Desktop (builds in a docker volume so fsync is real)
docker build -t arbordb-dev docker
powershell scripts/docker-test.ps1                    # build + ctest
powershell scripts/docker-test.ps1 -Cmd "/build/bin/arbordb /tmp/demo -c 'SELECT 1+1 FROM t;'"
```

Debug builds run under AddressSanitizer + UBSan. The B+Tree suite (~17 s) and the stress run are slow for that reason.

### Try it

```bash
./bin/arbordb /tmp/demo            # SQL shell; statements end with ';'
arbordb> CREATE TABLE emp (id INTEGER PRIMARY KEY, name VARCHAR(30), dept INTEGER, salary INTEGER);
arbordb> CREATE TABLE dept (id INTEGER PRIMARY KEY, name VARCHAR(30));
arbordb> INSERT INTO emp VALUES (1,'alice',1,120),(2,'bob',1,100),(3,'dave',2,80);
arbordb> INSERT INTO dept VALUES (1,'Engineering'),(2,'Sales');
arbordb> SELECT d.name, COUNT(*), AVG(e.salary) FROM emp e JOIN dept d ON e.dept = d.id GROUP BY d.name ORDER BY d.name;
arbordb> EXPLAIN SELECT name FROM emp WHERE id >= 2 AND id < 5;
```

```bash
./scripts/crash_recovery_test.sh   # kill -9 mid-transaction, restart, assert (3 scenarios, see below)
./bin/concurrency_stress_test --transactions 50 --iterations 10000
./bin/arbordb_server /tmp/demo 5433 &  ./bin/arbordb_client 127.0.0.1 5433   # custom TCP protocol (src/net/protocol.h)
```

## Supported SQL

`CREATE TABLE` (`INTEGER`, `DOUBLE`, `VARCHAR(n)`, `BOOLEAN`, `PRIMARY KEY` on an INTEGER column) · `CREATE [UNIQUE] INDEX` · `DROP TABLE` · `INSERT … VALUES` · `UPDATE … SET … WHERE` · `DELETE … WHERE` · `SELECT` with expressions, `WHERE` (`AND OR NOT`, comparisons, `IS [NOT] NULL`, `BETWEEN`, `IN (list)`, arithmetic), `[INNER] JOIN … ON`, comma/`CROSS JOIN`, aliases, self-joins, `GROUP BY`, `COUNT/SUM/MIN/MAX/AVG`, `HAVING`, `ORDER BY` (expression, alias, ordinal; NULLs first), `LIMIT` · `BEGIN/COMMIT/ROLLBACK` · `CHECKPOINT` · `EXPLAIN SELECT`. NULLs follow SQL three-valued logic. No subqueries, views, outer joins, `DISTINCT`, `OFFSET`, `LIKE`.

## How the hard parts work

**Storage.** 4 KB pages (`kPageSize`), common header `{page_id, lsn}`. Heap pages are slotted pages: slot array grows forward, tuple bytes backward. Slots are never reused and a delete is a tombstone, so every change can be undone without needing free space (`compact` is explicit and never called automatically). `DiskManager` keeps a free-page list in page 0.

**Buffer pool.** Fixed frames, pin counts, dirty bits, flush-on-evict. `LRUKReplacer` (K=2) evicts by largest backward K-distance, so a one-off sequential scan cannot evict a hot working set; tests assert the victim differs from plain LRU. `FetchPage`/`NewPage` return `nullptr` when every frame is pinned.

**B+Tree.** Nodes are buffer-pool pages. Insert splits leaves and internal nodes (growing the root); delete borrows from a sibling or merges (shrinking the tree one level at a time). Leaves are chained for range scans/iterators. `CheckInvariants()` verifies ordering, occupancy bounds, uniform depth and the leaf chain, and the test suite calls it after every single operation.

**WAL and ARIES.** Every page change is a logged *physiological* operation (insert / tombstone / update with before+after images / page init / link). The write-ahead rule is enforced in `BufferPoolManager`: before a page is written, the log is flushed through the page's LSN. Commit flushes the log through the COMMIT record. Recovery:
1. *Analysis* scans from the last checkpoint's `redo_start_lsn`, rebuilding the active-transaction and dirty-page tables.
2. *Redo* repeats history for committed and uncommitted work alike, gated by `page.lsn < record.lsn`.
3. *Undo* rolls back every loser through its `prev_lsn` chain, writing a CLR per undone record. CLRs carry `undo_next_lsn`, so a crash during undo resumes without undoing anything twice (tested: crash after 4 of 10 CLRs, recover again, exactly 6 more are written).

**Where fsync happens** (the places "looks durable" and "is durable" can diverge): `LogManager::Flush`/`FlushTo` (commit and the write-ahead rule; concurrent committers share one fsync), `TransactionManager::Checkpoint` (data file, after flushing all dirty pages), `TableHeap::CreateFirstPage` (a new table's first page, since DDL is not logged). Nothing else syncs.

**Concurrency.** `LockManager` is a real lock table: per-row queues, S/X modes, S→X upgrades, FIFO fairness so writers are not starved. `TwoPLManager` holds every lock until commit/abort (strict 2PL), so no transaction reads uncommitted data and aborts are always safe to undo. Deadlocks: whenever a transaction is about to block, the wait-for graph is rebuilt and searched for cycles (deterministic DFS); the **youngest** transaction in each cycle is aborted and its `Lock()` returns false. Detection is event-driven, so a deadlock resolves immediately and never waits on a timeout.
What is proven by `test_concurrency` / `concurrency_stress_test`: no dirty reads (a reader blocks on a writer's X lock; after a writer rollback it sees the original value), no lost updates (read-then-write counter), repeatable reads of rows already read, and a bank invariant (concurrent transfers + full-table audits) that always holds. The default stress run (50 threads, 10 000 transactions) resolved ~10 000 deadlocks with zero violations. It takes minutes in the Docker/ASan environment because each commit is an fsync.

**Query layer.** Hand-written lexer and recursive-descent parser (one method per grammar rule, see the grammar in `parser.h`). The binder resolves and qualifies every column against the catalog and builds a logical plan. The **rule-based** optimizer applies: predicate pushdown → join reordering (smaller input first; heap page count is the only size signal) → projection pushdown → access-path selection (`IndexScan` for `col =/</<=/>/>= const` on an indexed INTEGER column, `HashJoin` when there is an equi-condition, else `NestedLoopJoin`). It is explicitly not cost-based: there are no statistics or cardinality estimates. `EXPLAIN` prints the plan, and `tests/sql/plans.test` asserts plan shapes.

**Catalog.** A dedicated text file (`catalog.meta`), rewritten atomically after each DDL statement; not WAL-logged. Indexes are not persisted at all: they live in a scratch file that is truncated at startup and rebuilt from the recovered heaps.

## Design patterns (and where they are in the code)

| Pattern | Class(es) | Why |
|---|---|---|
| Strategy | `Replacer` / `LRUKReplacer` (`src/buffer`); `ConcurrencyControlManager` / `TwoPLManager` (`src/concurrency`) | swap eviction policy or concurrency scheme without touching callers (an `MVCCManager` would slot in beside `TwoPLManager`) |
| Iterator | `AbstractExecutor` with `Init()`/`Next()` and every operator under `src/query/executor`; `BPlusTree::Iterator` | Volcano composition: operators don't know their children's types |
| Visitor | `ExprVisitor` / `ExprWalker` (`src/query/expression.h`): column collection, aggregate detection, binder scope checking, EXPLAIN printing | tree-walking logic lives outside the node classes |
| Factory | `ExecutorFactory::Create` | the single plan-node → executor mapping |
| Command (optional) | `Statement` variant + `Server` (a request becomes parsed statement objects run on the connection's `Session`) | mirrors the request/command split in the network layer |
| Singleton | **deliberately not used as a global.** `BufferPoolManager` and `Catalog` are each created exactly once and owned by `Database` | ARCHITECTURE.md §9 lists them as narrow singletons; a global accessor would make it impossible to open two databases in one test process, so "one instance per database" is enforced by ownership instead |

## Known gaps (honest list)

- **Isolation level.** Row-level strict 2PL gives no dirty reads, no lost updates and repeatable reads for rows that exist. There are **no predicate/range locks**, so phantoms are possible (a scan can miss or see a row inserted by a concurrent transaction). So this is *not* full serializability for arbitrary workloads; the stress test proves the invariants above for a fixed set of rows.
- **Index scans are weaker than sequential scans.** Index entries are not locked or versioned, so an index lookup can miss a row another transaction has deleted but not yet committed. Indexes are unique (INTEGER column only), and an aborted delete followed by a concurrent insert of the same key can make the abort's index repair fail.
- **Index pages are not WAL-logged.** Rolled back in memory via per-transaction undo hooks, rebuilt from the heap after a crash (cost: startup is O(rows) per index).
- **Coarse B+Tree latching**: one mutex per tree; no latch crabbing. Iterators copy a leaf at a time and are not isolated from concurrent modification.
- **Catalog and DDL are not transactional or logged.** A crash mid-DDL can leak a page. `DROP TABLE` leaks the table's pages on purpose (redo of old log records could otherwise corrupt reused pages). DDL takes an exclusive engine lock but does not protect an open explicit transaction *between* its statements.
- **Statements are not atomic on their own:** any execution error or deadlock rolls back the whole transaction (PostgreSQL-style), rather than undoing just the failing statement.
- **Sharp checkpoints** (flush every dirty page) instead of fuzzy checkpoints; the log is never truncated or archived; recovery undoes losers one transaction at a time rather than interleaved by LSN (equivalent here because 2PL keeps losers on disjoint rows).
- **Heap pages never compact automatically**, so deleted space is not reused until an explicit `SlottedPage::Compact()`. Tuples must fit in one 4 KB page.
- **Page allocation is not logged.** The free list is rewritten but not fsynced on each allocation; the file length is trusted on open.
- **Durability model tested:** process kill (`kill -9`) with the OS surviving. Power-loss torn pages are not handled (no full-page images / doublewrite).
- **Optimizer** has no statistics; the heap-page-count size heuristic is crude. Hash join keeps the build side in memory.
- **MVCC (M7 stretch) is not implemented.** The `ConcurrencyControlManager` interface is the seam for it.
- The TCP server binds to localhost and has no authentication or TLS.
- Platform: Linux only (POSIX I/O is confined to `src/storage/disk/`, `src/recovery/log_manager.cpp` and `src/net/`).

## What I'd do at scale

Latch-crabbing (or Bw-tree/OLC) B+Trees so index operations scale across cores; logged index changes (nested top actions) instead of rebuild-on-start; key-range / next-key locking for real serializability, or MVCC with garbage collection (version chains, snapshot reads) so readers never block writers; fuzzy checkpoints plus log archiving/truncation; group commit with a dedicated log writer; full-page images or a doublewrite buffer for torn-page safety; a real cost-based optimizer with histograms and join-order search; a buffer pool with sharded latches and async I/O (`io_uring`); `VARCHAR` overflow pages; deadlock victim choice by work done instead of age; replication and distribution on top of the log.

## Layout

```
src/storage/{disk,page,table}   DiskManager, Page/SlottedPage, TableHeap
src/buffer                      BufferPoolManager, Replacer, LRUKReplacer
src/index                       BPlusTree (header-only template)
src/recovery                    LogRecord, LogManager, RecoveryManager
src/concurrency                 Transaction, TransactionManager, LockManager, TwoPLManager
src/catalog                     Value/Schema/Tuple, Catalog
src/query/{parser,binder,optimizer,executor}, expression.h, plan.h
src/db                          Database + Session (the whole engine behind one object)
src/net                         TCP protocol, Server, Client
tools/                          crash_driver, concurrency_stress_test, arbordb_server, arbordb_client
tests/ (+ tests/sql, tests/support)   unit tests, SQL fixtures, shared stress workloads
```
