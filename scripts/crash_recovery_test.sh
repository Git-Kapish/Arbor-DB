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

echo "== scenario 3: SQL shell killed with -9 in the middle of an explicit transaction =="
ARBORDB="${ARBORDB:-$(dirname "$DRIVER")/arbordb}"
if [ -x "$ARBORDB" ]; then
  D="$WORK/s3"; mkdir -p "$D"
  mkfifo "$WORK/in3"
  "$ARBORDB" "$D" < "$WORK/in3" > "$WORK/out3" 2>&1 &
  PID=$!
  exec 3> "$WORK/in3"
  cat >&3 <<'SQL'
CREATE TABLE acct (id INTEGER PRIMARY KEY, bal INTEGER);
INSERT INTO acct VALUES (1, 100), (2, 200), (3, 300);
UPDATE acct SET bal = 150 WHERE id = 1;
BEGIN;
UPDATE acct SET bal = 0 WHERE id = 2;
DELETE FROM acct WHERE id = 3;
INSERT INTO acct VALUES (4, 400);
CHECKPOINT;
INSERT INTO acct VALUES (5, 500);
SELECT 'in-flight-ready' FROM acct WHERE id = 1;
SQL
  for _ in $(seq 1 100); do grep -q in-flight-ready "$WORK/out3" 2>/dev/null && break; sleep 0.1; done
  grep -q in-flight-ready "$WORK/out3" || { echo "shell never reached the in-flight state"; cat "$WORK/out3"; exit 1; }
  kill -9 $PID; wait $PID 2>/dev/null; exec 3>&-
  echo "killed arbordb (pid $PID) with an open transaction that had updated, deleted and inserted rows"
  q() { "$ARBORDB" "$D" -c "$1" | sed -n '3p' | tr -d ' '; }
  "$ARBORDB" "$D" -c "SELECT COUNT(*) FROM acct;" | grep -E "recovery|^-- " || true
  check() { [ "$(q "$1")" = "$2" ] && echo "  ok: $1 -> $2" || { echo "  FAIL: $1 -> got '$(q "$1")', want '$2'"; fail=1; }; }
  check "SELECT COUNT(*) FROM acct;" 3
  check "SELECT SUM(bal) FROM acct;" 650            # 150 + 200 + 300: committed update kept, in-flight changes gone
  check "SELECT bal FROM acct WHERE id = 1;" 150    # committed update (autocommit) survived
  check "SELECT bal FROM acct WHERE id = 2;" 200    # in-flight update rolled back
  check "SELECT bal FROM acct WHERE id = 3;" 300    # in-flight delete rolled back
  check "SELECT COUNT(*) FROM acct WHERE id >= 4;" 0   # in-flight inserts gone (and the rebuilt index agrees)
else
  echo "arbordb binary not found, skipping scenario 3"
fi

if [ $fail -eq 0 ]; then echo "PASS: committed data survived, uncommitted data did not"; else echo "FAIL"; fi
exit $fail
