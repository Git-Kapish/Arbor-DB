#!/usr/bin/env bash
# Configure, build and test. Used by CI and by scripts/docker-test.ps1 so both run the same thing.
#   scripts/ci-build.sh <build_dir> <Debug|Release|RelWithDebInfo> <auto|address|thread|none> [ctest args...]
set -euo pipefail
BUILD_DIR="${1:?build dir}"; TYPE="${2:?build type}"; SAN="${3:?sanitizer}"; shift 3
SRC="$(cd "$(dirname "$0")/.." && pwd)"
cmake -S "$SRC" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$TYPE" -DARBOR_SANITIZER="$SAN" > /dev/null
if ! cmake --build "$BUILD_DIR" -j"$(nproc)" > "$BUILD_DIR/build.log" 2>&1; then
  grep -E -B2 -A8 'error' "$BUILD_DIR/build.log" | head -100
  exit 1
fi
grep -E 'warning' "$BUILD_DIR/build.log" | head -20 || true
cd "$BUILD_DIR"
# ThreadSanitizer: fail the run on any report, and print both stacks of a lock-order inversion.
export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=0 exitcode=66 second_deadlock_stack=1}"
# ThreadSanitizer (GCC 11) aborts with "unexpected memory mapping" under high-entropy ASLR, which Linux hosts and
# Docker enable by default. Disabling ASLR for the test processes avoids it; this needs a container started with
# --security-opt seccomp=unconfined (the default seccomp profile forbids the personality() call setarch uses).
PREFIX=""
if [ "$SAN" = "thread" ]; then PREFIX="setarch $(uname -m) -R"; fi
$PREFIX ctest --output-on-failure "$@"
