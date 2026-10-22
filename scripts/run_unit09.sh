#!/bin/sh
set -e
make all
echo; echo "== 9.1  the operator pipeline: its cost per row, and per query =="
./bin/u09_pipeline_bench
echo; echo "== 9.2-9.4  the join grammar, both joins, the differential test =="
./bin/u09_join_test
echo; echo "== 9.5  forensic trap: one bucket swallows every key =="
./bin/u09_hash_collapse_demo
echo; echo "== 9.6  payoff: nested loop vs hash join (about 4 minutes; n = 100,000 is most of it) =="
./bin/u09_join_bench
./bin/u09_walk_bench   # why the last row of the experiment is slower per pair
