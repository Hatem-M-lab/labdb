#!/bin/sh
set -e
make all
echo; echo "== 8.3  SQL end to end: build, query, reopen, semantic errors =="
./bin/u08_sql_test
echo; echo "== 8.4  forensic trap: a range query answered by a point lookup =="
./bin/u08_key_range_demo
echo; echo "== 8.5  payoff: a full SQL session, measured =="
./bin/u08_sql_bench
