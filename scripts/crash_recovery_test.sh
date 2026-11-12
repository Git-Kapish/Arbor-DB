#!/usr/bin/env bash
# Real crash test: a child process is killed with SIGKILL, then a fresh process recovers the files.
#   1. deterministic scenario: committed txn (log only) + in-flight txn (partly flushed to disk) -> kill -9 -> recover
#   2. churn scenario: kill -9 at random moments while transactions commit; check durability + atomicity
# Usage: scripts/crash_recovery_test.sh [path/to/crash_driver]
set -u
DRIVER="${1:-${ARBOR_BIN:-build/bin}/crash_driver}"
[ -x "$DRIVER" ] || DRIVER=/build/bin/crash_driver
[ -x "$DRIVER" ] || { echo "crash_driver not found (build first)"; exit 2; }
WORK=$(mktemp -d /tmp/arbor_crash.XXXXXX)   # a real local filesystem; fsync semantics matter here
trap 'rm -rf "$WORK"' EXIT
fail=0

echo "== scenario 1: kill -9 with an in-flight transaction =="
D="$WORK/s1"; mkdir -p "$D"
"$DRIVER" setup "$D" > "$WORK/s1.out" 2>&1 &
PID=$!
for _ in $(seq 1 100); do grep -q READY "$WORK/s1.out" 2>/dev/null && break; sleep 0.1; done
grep -q READY "$WORK/s1.out" || { echo "driver never became ready"; cat "$WORK/s1.out"; exit 1; }
kill -9 $PID; wait $PID 2>/dev/null
echo "killed pid $PID mid-transaction"
for attempt in 1 2; do   # second run proves recovery is idempotent (the first one ended without a clean shutdown too)
  if "$DRIVER" verify "$D"; then :; else fail=1; fi
done

echo "== scenario 2: random-time kills during a commit stream (5 rounds) =="
for round in 1 2 3 4 5; do
  D="$WORK/c$round"; mkdir -p "$D"
  "$DRIVER" churn "$D" > "$WORK/c$round.out" 2>&1 &
  PID=$!
  sleep "0.$((RANDOM % 8 + 2))"
  kill -9 $PID; wait $PID 2>/dev/null
  ACKED=$(grep -c '^ACK' "$WORK/c$round.out" || true)
  echo -n "round $round: killed after $ACKED acked commits -> "
  "$DRIVER" verify-churn "$D" "$ACKED" || fail=1
done

if [ $fail -eq 0 ]; then echo "PASS: committed data survived, uncommitted data did not"; else echo "FAIL"; fi
exit $fail
