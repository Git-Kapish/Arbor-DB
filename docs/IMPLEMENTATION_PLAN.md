# ArborDB — Implementation Plan

## Context
ArborDB is a from-scratch, single-node relational DB engine in C++17 (PRD.md, ARCHITECTURE.md, AGENTS.md, TECH_STACK.md). `D:\Projects\Arbor DB` currently holds only those four docs and is not a git repo. Goal of this step: turn the docs into an ordered build plan, set up the repo, and push to https://github.com/Git-Kapish/Arbor-DB.git. Milestones are strictly sequential (M1→M6, M7 stretch); none is "done" until its tests pass (AGENTS.md §2, §6).

## Environment (decided)
- Build/test in **WSL Ubuntu** (matches the docs: pread/pwrite, fsync, real `kill -9`, bash scripts). Windows MinGW is GCC 6.3 (no real C++17) and there's no cmake, so native Windows is out.
- One-time user step (needs sudo, I can't do it): `wsl -d Ubuntu -- sudo apt install -y build-essential cmake libgtest-dev`. I'll fall back to CMake `FetchContent` for GoogleTest only if the apt package is unavailable.
- Work happens on the Windows path, accessed from WSL at `/mnt/d/Projects/Arbor DB`. **Risk:** `/mnt/d` has slow I/O and weaker fsync semantics; durability/crash tests should run against a tmpfs/ext4 path (e.g. `/tmp/arbordb-test`) via a configurable data dir, not the `/mnt/d` tree.

## Step 0 — Repo setup + push (first thing after approval)
1. `git init -b main`, `git remote add origin https://github.com/Git-Kapish/Arbor-DB.git` (`gh` is already authed as Git-Kapish).
2. Add `.gitignore` (`build/`, `*.db`, `*.log`, IDE dirs) and this plan as `docs/IMPLEMENTATION_PLAN.md`.
3. Commits, short human messages, **no `feat:`-style prefixes and no Co-Authored-By/Claude trailer** (user instruction overrides the default attribution):
   - `add PRD, architecture, agent and tech stack docs`
   - `add implementation plan`
   - `cmake skeleton with gtest wired up`
4. `git push -u origin main`. Check the remote is empty first (`git ls-remote`); if it has history, stop and ask rather than force-pushing.
5. Same rule for every later commit: one commit per working milestone (AGENTS.md §2), messages like `slotted page insert/delete/compact`, `buffer pool with lru-k`.

## Skeleton (Step 0 third commit)
Create the AGENTS.md §4 layout: `src/{storage/{disk,page},buffer,index,recovery,concurrency,catalog,query/{parser,binder,optimizer,executor},net}`, `tests/`, `scripts/`. Root `CMakeLists.txt` (≥3.16, C++17, `-Wall -Wextra`, ASan/UBSan option for Debug), one static lib `arbordb_core`, one gtest executable per `tests/test_*.cpp` registered with CTest. `src/common/config.h` holds named constants (`kPageSize=4096`, `kLRUKValue=2`, id typedefs `page_id_t`, `frame_id_t`, `txn_id_t`, `lsn_t`, `RID`) per AGENTS.md §7. README gets a "Known Gaps" section from day one.

## Milestones (each ends: tests green → commit → report)

### M1 — Storage (`storage/disk`, `storage/page`)
- `DiskManager`: `ReadPage/WritePage` via `pread/pwrite`, `AllocatePage/DeallocatePage`, free-list in a dedicated header page so freed pages are reused; `fsync` isolated here. Platform-specific calls stay in this directory.
- `Page` (4KB, header: page_id, lsn, free-space ptr, slot count) and `SlottedPage`: slot array from front, tuples from back, delete = tombstone, lazy compaction. Insert returns `RID`.
- Tests (`test_disk_manager.cpp`, `test_slotted_page.cpp`): byte-identical read-back after close/reopen; free-list reuse; insert/delete/compact; **page-full edge case**; variable-length tuples.

### M2 — Buffer pool (`buffer/`)
- `Replacer` interface (Strategy) + `LRUKReplacer` (K=2; frames with <K accesses have +inf distance, ties broken by earliest first access; evictable flag per frame).
- `BufferPoolManager`: page table, free list, pin counts, dirty flag, flush-on-evict, `FetchPage/NewPage/UnpinPage/FlushPage/DeletePage`; returns `nullptr` when every frame is pinned (ARCH §10). RAII `PageGuard` so callers can't leak pins. Hook point for the WAL rule (`LogManager*` optional, wired in M4).
- Tests: pin beyond capacity → correct LRU-K victim asserted (not first/last arbitrarily); sequential-scan resistance test (hot page survives a scan, which plain LRU would lose); pinned page never evicted; all-pinned → `nullptr`; dirty page persisted on evict; multithreaded pin/unpin stress.

### M3 — B+Tree (`index/`)
- Templated on key + comparator, value = `RID`; internal/leaf nodes are buffer-pool pages with layout structs; leaf `next_page_id` chain; `BPlusTreeIterator`.
- Insert w/ leaf+internal split incl. root split; delete w/ redistribute-then-merge incl. root collapse (height shrink) and delete-only-key-in-leaf. Coarse tree-level latch — documented in code + README Known Gaps (latch crabbing = "at scale").
- Tests: random insert/delete vs `std::map` oracle with a **seeded** RNG (deterministic), invariants checked after every op (sorted, occupancy bounds, uniform leaf depth, leaf chain == in-order); forced root split; forced shrink by one level; range scans; duplicate/missing key handling.

### M4 — WAL + ARIES (`recovery/`)
- `LogRecord` types: BEGIN, UPDATE (page, offset, before/after, prev_lsn), COMMIT, ABORT, CLR (undo_next_lsn), CHECKPOINT (ATT + DPT). `LogManager`: in-memory buffer, `Flush()` = write + `fsync` (the single documented fsync point), `flushed_lsn`.
- Write-ahead rule: `BufferPoolManager` refuses to write a page until `log.flushed_lsn >= page.lsn` (forces a log flush first). Commit = flush log through COMMIT.
- `RecoveryManager`: **Analysis** (from last checkpoint; rebuild ATT/DPT), **Redo** (from min recLSN, skip when `page.lsn >= record.lsn`), **Undo** (loser txns, backward via prev_lsn, writing CLRs so a crash mid-undo is idempotent). Undo is not optional (AGENTS §8).
- Catalog persistence decision (ARCH §5 / PRD §9): dedicated catalog file, not WAL-logged initially, documented explicitly; must survive restart.
- Tests: in-process unit tests for each phase + CLR idempotence (crash during undo, recover twice, same result). **`scripts/crash_recovery_test.sh`**: runs a small driver binary, `kill -9` mid-transaction, restarts, asserts committed rows present and uncommitted absent. Runs against a non-`/mnt` data dir.

### M5 — Concurrency (`concurrency/`)
- `ConcurrencyControlManager` interface (Strategy) + `TwoPLManager` using a real `LockManager`: per-RID request queues, S/X (+ upgrade S→X), grant based on compatibility, `condition_variable` waits, all locks released only at commit/abort (strict 2PL). No `shared_mutex` standing in for it (AGENTS §8).
- Deadlock detection: wait-for graph, cycle detection (DFS, deterministic ordering — iterate sorted txn ids, not `unordered_map` order), victim = youngest txn in cycle; run on a background interval and on new wait edge. Victim's waiter wakes and throws/returns abort → `TransactionManager::Abort` undoes via logged before-images.
- `Transaction`/`TransactionManager` (begin/commit/abort, txn id allocation, undo hookup to M4).
- Tests + `bin/concurrency_stress_test --transactions 50 --iterations 10000`: bank-transfer invariant (sum constant → no lost updates), dirty-read probe (reader never sees uncommitted value), serializability check; deliberate 2-txn deadlock → exactly one aborted, other commits, no hang (test has a hard timeout).

### M6 — Query layer (`catalog/`, `query/`)
- `Catalog`/`TableInfo`/`IndexInfo` (`Schema`, `Column`, `Value` as `std::variant<int32,int64?,double?,string>` — start with INT, BIGINT, DOUBLE, VARCHAR, BOOLEAN), `TableHeap` (linked slotted pages), tuple serialization.
- Parser: hand-written lexer + recursive descent (`ParseSelectStatement`, `ParseWhereClause`, …) for CREATE TABLE, CREATE INDEX, DROP TABLE, INSERT, SELECT (WHERE, JOIN…ON, GROUP BY, aggregates COUNT/SUM/MIN/MAX/AVG, ORDER BY, LIMIT), UPDATE, DELETE. Precedence-climbing for expressions.
- Binder → logical plan (Visitor over AST). Optimizer: predicate pushdown, projection pushdown, smaller-table-first join order, index-scan selection when a predicate matches an index; documented as rule-based, not cost-based.
- Executors with `Init()/Next() -> std::optional<Tuple>`: SeqScan, IndexScan, Filter, Projection, NestedLoopJoin, HashJoin, Aggregation, Sort, Limit, plus Insert/Update/Delete. `ExecutorFactory` maps plan node → executor. Executors take row locks via the concurrency layer and write log records.
- Tests (`test_query_layer.cpp`): parser unit tests (incl. error cases), sqllogictest-style fixture files (SQL + hand-computed expected output) on a multi-table schema with ≥1 join and ≥1 aggregate; index scan vs seq scan equivalence.
- `src/main.cpp`: simple REPL so the demo works before any networking.

### M7 (stretch) — net + MVCC
Only after M1–M6 pass. TCP protocol + CLI client (Command pattern), then `MVCCManager` behind the existing Strategy interface with an honest isolation-level writeup. Out of scope for the first pass.

## Cross-cutting
- **Design patterns as named classes** (AGENTS §2): `Replacer`/`LRUKReplacer`, `ConcurrencyControlManager`/`TwoPLManager`, `AbstractExecutor` + concrete operators, AST `Visitor`, `ExecutorFactory`, `Catalog`/`BufferPoolManager` narrow singletons (accessor, but constructible for tests — tests must not share global state).
- **Determinism:** seeded RNGs, no iteration-order dependence on `unordered_map`, no sleeps as synchronization in tests.
- **Style:** RAII only, no raw `new/delete`, `#pragma once`, named constants, a unit-test file for every public class in the five core dirs.
- **README** (grows per milestone): every pattern named + why, Known Gaps (coarse B+Tree latch, catalog not WAL-logged, etc.), "what I'd do at scale" section.
- **Reporting:** after each milestone, run the full ctest suite and report failures verbatim; stop at a clean sub-boundary and say so if a session runs long — never silently drop merge/undo/etc.

## Verification
- Per milestone: `cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc) && ctest --output-on-failure` inside WSL (with ASan/UBSan on).
- M4: `./scripts/crash_recovery_test.sh` exits 0.
- M5: `./bin/concurrency_stress_test --transactions 50 --iterations 10000` exits 0 and the deadlock test finishes under its timeout.
- M6: fixture-based SQL tests pass; manual REPL demo of `CREATE TABLE → INSERT → SELECT … JOIN … GROUP BY`.
- Step 0: `git log --oneline` shows the short-message commits with no co-author trailers; `git ls-remote origin` shows `main` at the local HEAD.

## What this session will and won't do
After approval: Step 0 (repo, docs, skeleton, push). I'd then stop and confirm before starting M1, since each milestone is a sizeable, separately-verified chunk.
