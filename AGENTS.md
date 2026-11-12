# AGENTS.md — Instructions for Claude Code / coding agents working on ArborDB

Read `PRD.md` and `ARCHITECTURE.md` first — this file governs *how* to work, those govern *what* to build.

## 1. Project Snapshot
ArborDB is a single-node relational database engine in C++17, built layer by layer: disk-based storage → buffer pool → B+Tree index → WAL/ARIES recovery → concurrency control (2PL) → SQL query layer. No fixed timeline — milestones are ordered by hard dependency, and each must be genuinely complete, including tests, before the next begins.

## 2. Ground Rules for Agents
- **This project has no day-count deadline. Do not compress scope to "finish faster."** If a session is running long, stop at a clean sub-boundary within the current milestone and say so explicitly — don't silently skip a requirement (e.g. don't skip B+Tree merge-on-underflow and only implement split, don't skip the Undo phase of recovery) to appear "done."
- **Never mark a milestone complete without its correctness tests passing.** A milestone that "builds" but has no test proving its core claim (e.g. buffer pool eviction correctness, B+Tree split correctness, ARIES redo/undo correctness) is not complete — see Definition of Done (§6).
- **Every design pattern named in ARCHITECTURE.md §9 must actually appear in the code as a named, identifiable class/interface**, not just conceptually present.
- **Correctness first, then performance.** Get each layer provably correct before optimizing it. A fast, wrong buffer pool is worse than a slow, correct one at this stage.
- **Commit after each working milestone**, with clear messages (`feat(storage): slotted-page tuple insert/delete`, not `wip`).
- **If a scope tradeoff must be made** (e.g. coarse B+Tree latching instead of latch-crabbing, as noted in ARCHITECTURE.md §4), document it explicitly in the code and in `README.md`'s "Known Gaps" section — don't let it disappear silently.

## 3. Build & Test Commands
```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
ctest --output-on-failure

# Recovery / crash-test demo (scripted, not manual)
./scripts/crash_recovery_test.sh

# Concurrency / isolation-level stress test
./bin/concurrency_stress_test --transactions 50 --iterations 10000
```
Agents should run the full test suite after every non-trivial change and report failures immediately rather than layering new code on a broken build.

## 4. Directory Layout (target)
```
arbordb/
├── PRD.md
├── ARCHITECTURE.md
├── AGENTS.md
├── TECH_STACK.md
├── README.md
├── CMakeLists.txt
├── src/
│   ├── storage/
│   │   ├── disk/            # DiskManager, page allocation/free-list
│   │   └── page/             # Page format, slotted-page tuple layout
│   ├── buffer/                # BufferPoolManager, Replacer interface, LRUKReplacer
│   ├── index/                   # Generic B+Tree (node split/merge/rebalance, iterator)
│   ├── recovery/                  # LogManager, LogRecord types, RecoveryManager (ARIES)
│   ├── concurrency/                 # LockManager, ConcurrencyControlManager (2PL, MVCC stretch)
│   ├── catalog/                       # Catalog, TableInfo, IndexInfo
│   ├── query/
│   │   ├── parser/                      # Recursive-descent SQL parser, AST
│   │   ├── binder/                       # Name resolution against Catalog
│   │   ├── optimizer/                     # Rule-based rewrite passes
│   │   └── executor/                       # Volcano-model physical operators
│   ├── net/                                 # (stretch) TCP wire protocol, CLI client
│   └── main.cpp
├── scripts/
│   └── crash_recovery_test.sh
└── tests/
    ├── test_disk_manager.cpp
    ├── test_buffer_pool.cpp
    ├── test_bplus_tree.cpp
    ├── test_recovery.cpp
    ├── test_concurrency.cpp
    └── test_query_layer.cpp
```

## 5. Suggested Agent/Task Split
Unlike MiniCache's mostly-parallel module split, ArborDB's layers are **strictly sequential dependencies** — each layer is built on top of the one before it, so parallelizing across sessions only works within a layer, not across layers:

1. **Storage Agent** (M1) — `storage/`. No dependencies. Must finish and be tested before Buffer Pool Agent starts.
2. **Buffer Pool Agent** (M2) — `buffer/`. Depends on Storage Agent's `DiskManager` interface.
3. **Index Agent** (M3) — `index/`. Depends on Buffer Pool Agent's `BufferPoolManager` interface.
4. **Recovery Agent** (M4) — `recovery/`. Depends on Storage + Buffer Pool (needs to intercept page flushes for the write-ahead rule).
5. **Concurrency Agent** (M5) — `concurrency/`. Depends on Index Agent (locks are acquired around tuple/index access) and Recovery Agent (undo needs the concurrency layer's before-images).
6. **Query Agent** (M6) — `query/`. Depends on everything above — the executor calls through the concurrency layer into the index/storage layers.
7. **Net/MVCC Agent** (M7, stretch) — depends on Query Agent.

Within a layer, the parser/binder/optimizer/executor sub-modules of the Query Agent's work (M6) *can* be parallelized once the `Catalog` and logical-plan types are defined, since each stage's contract is the logical/physical plan tree, not the layers below.

## 6. Definition of Done (per milestone, mirrors PRD.md §8)
- **M1 done** when: pages can be written and read back byte-identical across a process restart; slotted-page insert/delete/compact has passing tests including a page-full edge case.
- **M2 done** when: a test pins more pages than the pool's capacity and asserts the correct page is evicted (LRU-K, not the first/last one arbitrarily); a test proves a pinned page is never evicted.
- **M3 done** when: a test suite inserts/deletes a large, randomized key sequence and verifies the tree remains correctly ordered and balanced after every operation — including a case that forces a root split and a case that forces the tree to shrink by one level via merges.
- **M4 done** when: `scripts/crash_recovery_test.sh` kills the process mid-transaction and the restarted engine passes assertions that committed data survived and uncommitted data did not.
- **M5 done** when: a stress test with concurrent transactions proves no dirty reads / no lost updates under strict 2PL, and a deliberately constructed deadlock scenario is detected and resolved (one transaction aborts) rather than hanging.
- **M6 done** when: the full SQL subset in PRD.md §5.7 runs correctly against a multi-table schema with at least one join and one aggregate query, verified against hand-computed expected results.

## 7. Code Style
- C++17, RAII throughout, no raw `new`/`delete`.
- Header guards via `#pragma once`.
- Every public class in `storage/`, `buffer/`, `index/`, `recovery/`, `concurrency/` gets a unit test file before being considered done — no exceptions, these are the layers where a silent bug corrupts data.
- Prefer explicit, named constants for magic numbers (`kPageSize`, `kLRUKValue`) over inline literals.

## 8. What NOT to Do
- Don't skip the Undo phase of recovery "for now" — a WAL implementation that only does Redo isn't ARIES and doesn't actually protect against the failure mode this project claims to solve.
- Don't implement B+Tree insert without delete, or split without merge — an index that only grows is not demonstrating the real data structure.
- Don't reach for `std::shared_mutex` everywhere as a substitute for the actual `LockManager` — the point of M5 is building real lock management with deadlock detection, not relying on language-level primitives to fake it.
- Don't use a parser-generator (ANTLR, yacc/bison) for the SQL parser — the hand-written recursive-descent parser is explicitly part of the scope (PRD.md §5.7).
