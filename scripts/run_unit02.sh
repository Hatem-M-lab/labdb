#!/bin/sh
# Build and run everything Unit 2 claims. From the repository root:
#   sh scripts/run_unit02.sh
set -e
make all

echo "=== correctness: B+Tree vs std::map, splits, persistence ==="
./bin/u02_btree_test
./bin/u02_split_regression_test

echo
echo "=== the forensic trap: the disappearing median ==="
./bin/u02_median_bug_demo

echo
echo "=== the payoff: point lookup pages read, and insert fill factor ==="
./bin/u02_search_bench
./bin/u02_insert_bench
