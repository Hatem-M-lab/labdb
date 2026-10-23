#!/bin/sh
set -e
make all
echo; echo "== 13.1-13.3  the log, transactions, recovery: one property at a time =="
./bin/u13_wal_test
echo; echo "== 13.4  crashing the engine at every write and sync, and kill -9 =="
./bin/u13_crash_test
echo; echo "== 13.4  the crash test, tested: seven planted bugs =="
sh scripts/u13_mutation_check.sh
echo; echo "== 13.5  forensic trap: the log that remembered too much =="
./bin/u13_stale_log_demo
echo; echo "== 13.1, 13.6  what durability costs =="
./bin/u13_durability_bench
echo; echo "== the old queries: Unit 8's benchmark against Unit 12's build =="
sh scripts/u13_before_after.sh
