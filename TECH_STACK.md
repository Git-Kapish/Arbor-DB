# TECH_STACK — ArborDB

## Language & Standard
- **C++17.** Same reasoning as MiniCache: no garbage collector, so page/memory lifetime management is something this project has to design deliberately (pin counts, RAII page guards) rather than delegate to a runtime — and that's a core part of what a database internals project is meant to demonstrate. `std::variant` for typed tuple values (int/varchar/etc.), `std::optional` for `Executor::Next()`'s "no more tuples" signal, structured bindings throughout.

## Build System
- **CMake (≥3.16)**, out-of-source builds, mirroring MiniCache for a consistent toolchain across both projects.

## Testing
- **GoogleTest** (matches MiniCache's choice) via **CTest**. Given how correctness-critical this project is (a subtly wrong B+Tree split or recovery bug corrupts data silently), lean on:
  - Deterministic unit tests for every layer (see AGENTS.md §6 Definition of Done).
  - Randomized/property-style tests for the B+Tree (random insert/delete sequences, checked against an `std::map` oracle for correctness) and for the concurrency stress tests.
  - A scripted end-to-end crash-recovery test (`scripts/crash_recovery_test.sh`) that actually kills the process, not just calls a "simulateCrash()" function — this catches bugs that only manifest with a real OS-level process termination.

## Storage & I/O
- Raw POSIX file I/O (`pread`/`pwrite`, `fsync`) for the `DiskManager` — no `mmap`-based storage engine and no embedded database/library. As with MiniCache's raw sockets, hand-rolling this is the point: the buffer pool and page management logic is the thing being demonstrated, and a framework would hide it.
- `fsync`/`fdatasync` used explicitly at the log-flush boundary (the write-ahead rule depends on the log actually being durable before dependent pages are flushed) — document exactly where fsync calls happen, since this is a common place where "looks correct" and "is actually durable" diverge.

## Concurrency Primitives
- `std::thread`, `std::mutex`, `std::condition_variable` for the buffer pool and lock manager's internal bookkeeping.
- The `LockManager`'s row-level locks are **application-level locks** (ArborDB's own data structure tracking who holds what), not `std::mutex` per row — this is what makes deadlock detection and 2PL semantics possible; a raw mutex has no concept of "shared vs exclusive" or "which transaction holds this."

## Query Layer
- Hand-written recursive-descent parser — no ANTLR/yacc/bison/Lex. Deliberate, per PRD.md §5.7 and AGENTS.md §8: writing the parser is part of the project's value, not incidental plumbing to route around.
- No third-party query planning or execution library — the Volcano-model executor is original.

## Networking (stretch, M7)
- Raw POSIX sockets, same rationale as MiniCache — if a client/server layer is built, it should not depend on a framework that would hide the wire-protocol work.

## Benchmarking / Validation
- Custom microbenchmarks per layer (buffer pool hit rate under different access patterns, B+Tree operation throughput, transaction throughput under 2PL) rather than a single end-to-end number — because this project's story is about *which layer* does what, and per-layer numbers support that narrative better than one aggregate figure.
- Consider validating SQL correctness against a small hand-written "expected results" fixture set per query, in the style of `sqllogictest` (no need to adopt the actual sqllogictest framework — the concept of fixture-based expected-output testing is what matters).

## Explicitly Avoided
- **No embedded database or storage engine library** (SQLite, LevelDB, RocksDB, LMDB) — using one would replace the exact layer this project exists to build.
- **No ORM, no existing SQL parser library** — same reasoning.
- **Boost** — avoided for the same minimal-dependency-footprint reason as MiniCache, with the possible documented exception of `Boost.Asio` if the stretch networking layer wants an event loop instead of thread-per-connection.

## Dev Environment Notes for Agents
- Target platform: Linux (POSIX file I/O + POSIX sockets, matching MiniCache). Note this explicitly if working on macOS/Windows — `pread`/`pwrite` and `fsync` semantics differ slightly across platforms and should be isolated behind `storage/disk/` rather than leaking into upper layers.
- No external services, API keys, or network access required to build/run/test — fully self-contained, same as MiniCache, which matters for sandboxed agent environments without egress.
- If continuity with MiniCache matters to you (shared toolchain, shared conventions), this project intentionally mirrors its build system, test framework, and "no third-party implementation of the core thing" philosophy — read MiniCache's `TECH_STACK.md` for the same rationale applied to a cache instead of a database.
