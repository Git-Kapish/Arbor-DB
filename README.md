# ArborDB

A single-node, disk-based relational database engine written from scratch in C++17: page storage, buffer pool (LRU-K), B+Tree index, ARIES-style WAL recovery, strict 2PL concurrency control, and a hand-written SQL parser/optimizer/executor.

Design docs: [PRD](PRD.md), [ARCHITECTURE](ARCHITECTURE.md), [TECH_STACK](TECH_STACK.md), [AGENTS](AGENTS.md). Build order and per-milestone plan: [docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md).

## Status
Skeleton only. Milestones M1 (storage) through M6 (query layer) are not started yet.

## Build (Linux / WSL)
```bash
sudo apt install -y build-essential cmake libgtest-dev
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
ctest --output-on-failure
```

## Known Gaps
_Filled in as scope tradeoffs are made (e.g. coarse B+Tree latching, catalog not WAL-logged)._

## Design patterns used
_Filled in per milestone._

## What I'd do at scale
_Filled in at the end._
