#!/bin/sh
# Build and run everything Unit 3 claims. From the repository root:
#   sh scripts/run_unit03.sh
set -e
make all

echo "=== correctness: deletion & range scans vs std::map ==="
./bin/u03_delete_test
./bin/u03_scan_regression_test

echo
echo "=== the forensic trap: the broken sibling chain ==="
./bin/u03_merge_chain_bug_demo

echo
echo "=== the payoff: range cost scales with the answer; deletion reclaims space ==="
./bin/u03_range_bench
./bin/u03_delete_bench
