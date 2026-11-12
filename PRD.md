# PRD — ArborDB (a disk-based relational database engine, built from scratch)

## 1. Summary
ArborDB is a single-node relational database engine implemented from first principles in C++17: a page-based disk storage layer, a buffer pool with real eviction, a B+Tree index, ARIES-style write-ahead logging with crash recovery, a pluggable concurrency control layer (strict two-phase locking, with MVCC as a stretch goal), and a SQL query layer (parser → binder → optimizer → Volcano-style executor). No embedded database, no third-party storage engine, no parser generator — every layer is hand-built.

This project has no fixed timeline. Depth and correctness take priority over speed of delivery; each milestone should be genuinely finished — including its tests — before moving to the next.

## 2. Goals
- G1: Data lives on disk in fixed-size pages, managed by a real buffer pool with pinning and eviction — not an in-memory map pretending to be a database.
- G2: Survive a crash at any point (including mid-transaction) with zero committed-data loss and zero uncommitted-data leakage, proven via ARIES-style WAL recovery.
- G3: Provide real transaction isolation — a stress test must demonstrate an actual isolation level (e.g. serializability under 2PL), not just "operations don't crash when run concurrently."
- G4: Support a real (if reduced) SQL subset: `SELECT`/`INSERT`/`UPDATE`/`DELETE`, `WHERE` predicates, joins, `GROUP BY`/aggregates, `ORDER BY`, `LIMIT` — executed through an actual query plan, not special-cased string matching.
- G5: A working B+Tree index with genuine split/merge/rebalance logic, used by the query executor for index scans.

## 3. Non-Goals (explicitly out of scope)
- Full SQL standard compliance (no subqueries beyond a basic case, no views, no stored procedures, no triggers).
- Distributed/multi-node operation — this is a single-node engine. (A distributed layer on top of this is a natural *next* project, not part of this one.)
- A cost-based optimizer with real statistics/histograms — a rule-based optimizer (predicate/projection pushdown, basic join reordering) is sufficient.
- Full MVCC with vacuum/garbage collection — if MVCC is attempted (stretch), a simplified version is acceptable as long as it's honestly documented as simplified.
- A production wire protocol (e.g. real Postgres wire protocol) — a custom, simple client/server protocol is enough for a live demo.

## 4. Users / Use Cases
- **Primary use case:** a portfolio-quality systems project demoable end-to-end: create a table, insert rows, run a `SELECT ... WHERE ... JOIN ... GROUP BY`, kill -9 the process mid-transaction, restart, and show recovery worked correctly.
- **Secondary use case:** a testbed for demonstrating specific database internals knowledge in interviews — being able to say "here's my B+Tree split logic" or "here's how my recovery manager replays the log" with real code behind it.

## 5. Functional Requirements

### 5.1 Storage layer
- Fixed-size pages (4KB, configurable), stored in a single heap file per table (or a page-per-table-with-extents model — document whichever is chosen).
- `DiskManager`: raw page read/write via `pread`/`pwrite`, page allocation/deallocation, free-list tracking.
- Page format: page header (page ID, LSN, free space pointers) + slotted-page layout for variable-length tuples.

### 5.2 Buffer pool
- `BufferPoolManager`: fixed-capacity pool of frames, pin/unpin counting, dirty-page tracking, flush-on-evict.
- Replacement policy: **LRU-K** (not plain LRU) — a deliberate, explainable choice over MiniCache's plain LRU/LFU, chosen because it resists sequential-scan cache pollution better than LRU. Implement as a `Replacer` interface (Strategy pattern) so the policy is swappable.

### 5.3 Indexing
- A generic B+Tree (`key type` templated or type-erased), built on top of the buffer pool (tree nodes are pages).
- Real insert/delete with node split (on overflow) and merge/redistribute (on underflow) — not a simplified "never merges" version.
- Point lookup, range scan (leaf-level linked list traversal), and an iterator interface usable by the query executor.

### 5.4 Write-ahead logging & recovery
- ARIES-style log records: `BEGIN`, `UPDATE` (with before/after images), `COMMIT`, `ABORT`, `CHECKPOINT`.
- Every page modification is logged before the page is allowed to be flushed to disk (write-ahead rule), enforced via LSN comparison.
- Recovery on startup runs the three ARIES phases: **Analysis** (rebuild transaction table + dirty page table from the log), **Redo** (replay all logged changes from the earliest needed LSN forward), **Undo** (roll back any transaction that was active at crash time).
- **Demonstrable requirement:** kill -9 mid-transaction, restart, and show via a scripted test that (a) committed transactions are fully present and (b) the in-flight transaction's partial writes are rolled back.

### 5.5 Concurrency control
- **Required:** Strict Two-Phase Locking (2PL) — a `LockManager` supporting shared/exclusive locks at row (tuple) granularity, with deadlock detection via a wait-for graph and cycle detection (or a timeout-based fallback, document the tradeoff).
- **Stretch:** MVCC as an alternative, swappable `ConcurrencyControlManager` implementation (Strategy pattern) — undo/version chains, snapshot-based reads. If implemented, clearly document which isolation levels it actually provides vs. which it claims to.
- **Demonstrable requirement:** a concurrent transaction stress test that proves a specific isolation level is actually enforced (e.g. no dirty reads, no lost updates) — not just "no crashes under load."

### 5.6 Catalog
- A `Catalog` component tracking table schemas (column names/types), and the indexes defined on each table. Not persisted via the general WAL initially is acceptable if explicitly documented — but must survive restart (either via its own bootstrap table or a dedicated catalog file).

### 5.7 Query layer
- **Parser:** hand-written recursive-descent parser for the supported SQL subset → AST. (No parser generator — the parser is part of what this project demonstrates.)
- **Binder:** resolves table/column names against the catalog, produces a bound/typed logical plan.
- **Optimizer:** rule-based — predicate pushdown, projection pushdown, and a simple join-order heuristic (e.g. smaller-table-first). Document this explicitly as rule-based, not cost-based.
- **Executor:** Volcano/iterator model — every physical operator (`SeqScan`, `IndexScan`, `Filter`, `Projection`, `NestedLoopJoin`, `HashJoin`, `Aggregation`, `Sort`, `Limit`) implements `Init()`/`Next()`, and operators compose by pulling from their children.

### 5.8 Client/server (stretch)
- A simple TCP wire protocol (custom, not Postgres-compatible) and a minimal CLI client, so the engine can be demoed interactively rather than only via unit tests.

## 6. Non-Functional Requirements
- **Durability correctness over raw speed** — this project's value is in correctness of the storage/recovery/concurrency layers, not in beating a production database's throughput.
- **Every core claim must be backed by a test that would fail if the claim were false** — e.g. "the buffer pool doesn't over-pin" needs a test that pins beyond capacity and asserts the correct eviction behavior, not just a passing happy-path test.
- **Reproducibility:** deterministic test suite — no test should be flaky due to unspecified iteration order over `unordered_map`, timing races, etc.

## 7. Success Metrics
1. A full `CREATE TABLE` → `INSERT` → `SELECT ... WHERE ... JOIN ... GROUP BY` session runs correctly end-to-end.
2. Kill -9 mid-transaction, restart, and a scripted test confirms correct ARIES recovery (committed data present, uncommitted data absent).
3. A concurrency stress test proves the claimed isolation level under strict 2PL, including a deadlock scenario that is correctly detected and resolved (one transaction aborted, not a hang).
4. B+Tree correctness proven via property-based or exhaustive tests covering split, merge, and rebalance — including edge cases (deleting the only key in a leaf, splitting the root).
5. README explicitly names every design pattern used and why, plus a "what I'd do at scale" section covering what a production engine adds beyond this scope.

## 8. Milestones
Milestones are ordered by hard dependency — each one requires the previous to be genuinely complete (including tests), not just "compiling."

- **M1 — Storage layer:** `DiskManager`, page format, slotted-page tuple storage.
- **M2 — Buffer pool:** `BufferPoolManager` + `LRU-K Replacer`, pin/unpin/flush correctness tests.
- **M3 — B+Tree index:** generic B+Tree on top of the buffer pool, full split/merge/rebalance, point + range query tests.
- **M4 — WAL & recovery:** ARIES logging, checkpoint, and the three-phase recovery algorithm, demonstrated via a scripted crash test.
- **M5 — Concurrency control:** `LockManager` with strict 2PL and deadlock detection, isolation-level stress tests.
- **M6 — Query layer:** parser → binder → rule-based optimizer → Volcano executor, covering the SQL subset in §5.7.
- **M7 (stretch) — Client/server + MVCC:** TCP wire protocol/CLI client, and/or an MVCC concurrency control implementation as an alternative to 2PL.

## 9. Open Questions / Decisions Deferred to Architecture Doc
- Exact page size and slotted-page layout details — ARCHITECTURE.md §2.
- Row-level vs. page-level locking granularity tradeoffs — ARCHITECTURE.md §6.
- Whether the catalog is WAL-logged or bootstrapped separately — ARCHITECTURE.md §5.
