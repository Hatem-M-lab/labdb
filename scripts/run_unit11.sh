#!/bin/sh
set -e
make all
echo; echo "== 11.1-11.4  indexes and the planner: every query under seven plans =="
./bin/u11_index_test
echo; echo "== 11.2  the differential test, tested: five planted bugs =="
sh scripts/u11_mutation_check.sh
echo; echo "== 11.1  what a range of the key costs =="
./bin/u11_range_bench
echo; echo "== 11.2  what a secondary index buys, and what it costs =="
./bin/u11_index_bench
echo; echo "== 11.3  when an index loses: the calibration and the race =="
./bin/u11_cost_bench
echo; echo "== 11.4  planning a join: the build side, and index loop or hash join =="
./bin/u11_join_bench
echo; echo "== 11.5  forensic trap: a run of duplicates split by a leaf =="
./bin/u11_split_run_demo
echo; echo "== 11.6  payoff: the session, replanned =="
./bin/u11_session_bench
