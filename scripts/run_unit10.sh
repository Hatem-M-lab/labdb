#!/bin/sh
set -e
make all
echo; echo "== 10.1-10.3  aggregates, GROUP BY and HAVING, ORDER BY and LIMIT: the differential test =="
./bin/u10_aggregate_test
echo; echo "== 10.3  what ORDER BY costs, and what a LIMIT saves =="
./bin/u10_sort_bench
echo; echo "== 10.4  forensic trap: a NULL counted as zero =="
./bin/u10_null_as_zero_demo
echo; echo "== 10.5  payoff: an analytics session, and the cardinality experiment =="
./bin/u10_group_bench
