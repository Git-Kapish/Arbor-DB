# ARCHITECTURE — ArborDB

## 1. High-Level Component Diagram

```
                     ┌───────────────────────┐
  CLI client / tests │      Query Layer        │
                     │  Parser → Binder →       │
                     │  Optimizer → Executor     │  (Volcano/iterator model)
                     └──────────┬───────────────┘
                                │ tuples in/out via operator Next()
                                ▼
                     ┌───────────────────────┐
                     │      Catalog            │  table schemas, index registry
                     └──────────┬───────────────┘
                                │
                     ┌──────────▼───────────────┐
                     │  Concurrency Control       │  LockManager (2PL) — Strategy: 2PL / MVCC
                     │  (row-level locks + wait-for graph, deadlock detection)
                     └──────────┬───────────────┘
                                │ acquires locks around page/tuple access
                     ┌──────────▼───────────────┐
                     │      B+Tree Index          │  generic key→RID, split/merge/rebalance
                     └──────────┬───────────────┘
                                │ reads/writes pages via buffer pool
                     ┌──────────▼───────────────┐
                     │   Buffer Pool Manager       │  LRU-K Replacer, pin/unpin, dirty tracking
                     └──────────┬───────────────┘
                                │ page fetch/flush
                     ┌──────────▼───────────────┐
                     │      Disk Manager           │  raw page read/write (pread/pwrite)
                     └──────────┬───────────────┘
                                │
                     ┌──────────▼───────────────┐
                     │   Heap files (on disk)      │  fixed-size pages, slotted-page tuples
                     └────────────────────────────┘

  Cross-cutting:
  ┌───────────────────────┐        ┌───────────────────────┐
  │   Log Manager (WAL)     │◀──────│  Recovery Manager        │  ARIES: Analysis → Redo → Undo
  │  BEGIN/UPDATE/COMMIT/    │       │  runs once at startup     │
  │  ABORT/CHECKPOINT        │       └───────────────────────┘
  └───────────────────────┘
       ▲ every page mutation logged here BEFORE the page may be flushed (write-ahead rule)
```

## 2. Storage Layer

### 2.1 Page format
- Fixed page size (default 4096 bytes), identified by a `page_id_t`.
- Page header: `page_id`, `lsn` (log sequence number of the last mutation applied to this page — the write-ahead rule is enforced by refusing to flush a page whose `lsn` exceeds the log's durably-flushed LSN), free-space pointer, slot count.
- **Slotted-page layout** for tuple storage: a slot array grows from the front of the page, tuple data grows from the back, meeting in the middle. This allows variable-length tuples and O(1) tuple deletion (mark slot as deleted, compact lazily) without shifting every subsequent tuple.

### 2.2 `DiskManager`
- Owns the raw file handle(s); exposes `ReadPage(page_id, out_buf)` / `WritePage(page_id, in_buf)` via `pread`/`pwrite`.
- Maintains a free-page list (either an in-page bitmap or a dedicated free-list page) so deleted pages are reused rather than leaking disk space.

## 3. Buffer Pool

```cpp
class Replacer {                      // Strategy pattern
public:
    virtual bool Victim(frame_id_t* out) = 0;
    virtual void Pin(frame_id_t) = 0;
    virtual void Unpin(frame_id_t) = 0;
    virtual ~Replacer() = default;
};
class LRUKReplacer : public Replacer { /* tracks last K accesses per frame; evicts the frame
                                            with the largest backward K-distance — approximates
                                            "least likely to be reused soon" better than plain LRU,
                                            which is fooled by a single sequential scan. */ };
```
- `BufferPoolManager::FetchPage(page_id)` — returns a pinned page, loading from disk via `DiskManager` on a pool miss, evicting via `Replacer` if the pool is full.
- Pin count semantics: a page cannot be evicted while pinned; callers must `UnpinPage` when done. This is the #1 source of subtle bugs in this kind of project — worth a dedicated stress test (see AGENTS.md Definition of Done).
- Dirty flag: set on any mutation; only dirty pages are written back on eviction/flush, and only after their `lsn` has been durably logged (write-ahead rule).

## 4. B+Tree Index

- Internal and leaf nodes are both stored as pages (reusing the buffer pool — the tree is "just" a specific page layout, not a separate memory system).
- **Insert:** descend to the target leaf, insert the key; if the leaf overflows, split it in half and insert a separator key into the parent (recursively, up to the root if needed — root split increases tree height by one).
- **Delete:** descend to the target leaf, remove the key; if the leaf underflows below a minimum occupancy threshold, attempt to redistribute from a sibling, or merge with a sibling if redistribution isn't possible (recursively, potentially decreasing tree height).
- **Range scan:** leaf nodes are linked (leaf → next-leaf pointer) so a range query walks leaves directly without re-descending from the root for each key.
- **Latching:** for M3, coarse-grained (whole-tree or root-to-leaf path) latching is acceptable and should be explicitly documented as a simplification; fine-grained latch-crabbing is a documented "what I'd do at scale" item, not required scope.

## 5. Catalog

- In-memory table of `TableInfo` (name, schema/column list, first page ID of the table's heap file) and `IndexInfo` (name, table, key schema, root page ID of the B+Tree).
- Bootstrapped from a small set of well-known system pages read at startup (a minimal "catalog is itself a table" bootstrap, or a dedicated catalog file — pick one and document it in code comments; both are legitimate, real systems differ on this).

## 6. Write-Ahead Logging & Recovery (ARIES)

### 6.1 Log records
`BEGIN(txn_id)`, `UPDATE(txn_id, page_id, offset, before_image, after_image, prev_lsn)`, `COMMIT(txn_id)`, `ABORT(txn_id)`, `CHECKPOINT(active_txn_table, dirty_page_table)`.

### 6.2 The write-ahead rule
A page may only be flushed to disk once its `lsn` has been durably written to the log file. This is what makes redo/undo correct after a crash — the log is always at least as up to date as the data.

### 6.3 Recovery — three ARIES phases, run once at startup
1. **Analysis** — scan the log from the last checkpoint forward, rebuilding the transaction table (which transactions were active) and the dirty page table (which pages had uncommitted changes) as of the crash.
2. **Redo** — replay every logged update from the earliest relevant LSN forward, regardless of whether the transaction committed — this repeats history to bring the database to exactly the state it was in at crash time.
3. **Undo** — roll back all transactions that were still active (not committed) at crash time, using the before-images logged in their `UPDATE` records, writing compensation log records (CLRs) so a second crash during undo doesn't redo the undo.

## 7. Concurrency Control

```cpp
class ConcurrencyControlManager {     // Strategy pattern
public:
    virtual bool AcquireLock(txn_id_t, RID, LockMode) = 0;
    virtual void ReleaseLock(txn_id_t, RID) = 0;
    virtual ~ConcurrencyControlManager() = default;
};
class TwoPLManager : public ConcurrencyControlManager { /* strict 2PL: all locks held until commit/abort */ };
class MVCCManager : public ConcurrencyControlManager   { /* stretch: version chains + snapshot reads */ };
```
- **Lock granularity:** row-level (tuple), via shared (read) and exclusive (write) locks tracked per `RID` (record ID = page_id + slot).
- **Deadlock detection:** build a wait-for graph from the lock manager's blocked-transaction state; run cycle detection periodically (or on each new wait edge); on a detected cycle, abort the youngest transaction in the cycle (a simple, defensible policy — document the alternative of aborting the "lowest cost" transaction as a scale-up item).
- **Strict 2PL specifically** (not just 2PL): all locks are held until the transaction commits or aborts, not released as soon as the growing phase ends — this is what gives strict 2PL its recoverability guarantee (no dirty reads possible, and undo is always safe).

## 8. Query Layer

```
SQL text ──▶ Parser ──▶ AST ──▶ Binder ──▶ Logical Plan ──▶ Optimizer ──▶ Physical Plan ──▶ Executor ──▶ tuples
           (recursive        (resolves        (rule-based:        (Volcano model:
            descent)          names against     predicate/          each node is
                               Catalog)          projection          Init()/Next())
                                                  pushdown, join
                                                  order heuristic)
```
- **Parser:** hand-written recursive-descent, one function per grammar rule (`ParseSelectStatement`, `ParseWhereClause`, etc.) — this is deliberately not a parser-generator (ANTLR/yacc) output, because writing it by hand is part of what the project demonstrates.
- **Binder:** walks the AST, resolves table/column references against the `Catalog`, attaches types, produces a logical plan tree.
- **Optimizer (rule-based):** predicate pushdown (move `WHERE` filters as close to the scan as possible), projection pushdown (only read needed columns), simple join-order heuristic (smaller estimated input first). Explicitly not cost-based — no cardinality estimation/statistics; document this as the reason.
- **Executor (Volcano/iterator model):** every physical operator implements `Init()` and `Next() -> optional<Tuple>`; a query plan is a tree of operators, and the top operator is pulled repeatedly by the caller until it returns nothing. `HashJoin` builds a hash table on the smaller side and probes with the larger; `NestedLoopJoin` is the fallback for non-equi joins.

## 9. Design Patterns Used

| Pattern | Where | Why |
|---|---|---|
| Strategy | `Replacer` (LRU-K, pluggable), `ConcurrencyControlManager` (2PL / MVCC) | swap core algorithms without touching call sites |
| Iterator | Every physical executor operator (`Init`/`Next`) | Volcano-model composition — operators don't know their children's concrete type |
| Visitor | AST traversal in the binder and optimizer's rewrite passes | separates tree-walking logic from node definitions |
| Factory | `ExecutorFactory` building the right executor from a physical plan node | centralizes the plan-node → executor mapping |
| Singleton (narrow, justified) | `Catalog`, `BufferPoolManager` | one instance per running database process |
| Command (optional, if a client/server is built) | Client request → parsed statement object | mirrors MiniCache's approach for consistency |

## 10. Failure Modes Considered
- Crash between a page write and its log flush → prevented structurally by the write-ahead rule, not just documented as a risk.
- Crash during undo itself (a second failure) → handled via compensation log records (CLRs), so undo is itself redoable/idempotent.
- Deadlock between two transactions → detected via wait-for graph, resolved by aborting one, not left to time out silently.
- B+Tree root split/merge concurrently with other operations → for M3 scope, coarse latching sidesteps this; documented as a "what I'd do at scale" item (latch crabbing) rather than silently ignored.
- Buffer pool exhaustion (every frame pinned, none evictable) → `FetchPage` must return an explicit failure/`nullptr`, not undefined behavior — callers (especially the query executor) must handle this.
