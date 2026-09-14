#!/bin/sh
# Reproduce every Unit 6 result from a clean build. Run from the repo root.
set -e
make all
echo; echo "== 6.2  catalog: 3 tables, cold reopen, exact-match names =="
./bin/u06_catalog_test
echo; echo "== 6.4  forensic trap: a table that was never created =="
./bin/u06_prefix_collision_demo
echo; echo "== 6.5  payoff: many tables, reached by name =="
./bin/u06_catalog_bench
