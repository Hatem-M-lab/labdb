#!/bin/sh
# Reproduce every Unit 5 result from a clean build. Run from the repo root.
# Fixed seeds: row counts, page counts, null counts, and the zero mismatches
# match to the digit; only wall-clock timings vary with hardware.
set -e
make all

echo
echo "== 5.1/5.2  record codec: types, nulls, text, projection =="
./bin/u05_record_test

echo
echo "== 5.3  the table heap: insert, RID, cold reopen, erase =="
./bin/u05_heap_test

echo
echo "== 5.4  forensic trap: a column shifted by a null =="
./bin/u05_null_shift_demo

echo
echo "== 5.5  payoff: a key finds a typed row, half a million times =="
./bin/u05_end_to_end_bench
