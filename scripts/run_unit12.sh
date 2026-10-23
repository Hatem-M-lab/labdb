#!/bin/sh
set -e
make all
echo; echo "== 12.2-12.4  the batch path against the row path: every query, six batch sizes =="
./bin/u12_batch_test
echo; echo "== 12.3-12.4  the differential test, tested: seven planted bugs =="
sh scripts/u12_mutation_check.sh
echo; echo "== 12.1-12.2  a scanned row, one step at a time =="
./bin/u12_anatomy_bench
echo; echo "== 12.1-12.2  ... counted in instructions =="
sh scripts/u12_anatomy_counts.sh
echo; echo "== 12.3  Unit 9's pipeline both ways; the filter's curve; the batch size =="
./bin/u12_pipeline_bench
echo; echo "== 12.3  ... the pipeline counted in instructions =="
sh scripts/u12_pipeline_counts.sh
echo; echo "== 12.4  grouping, sorting and joining in batches =="
./bin/u12_group_bench
echo; echo "== 12.5  forensic trap: an empty batch read as the end =="
./bin/u12_empty_batch_demo
echo; echo "== 12.6  when an index loses, now: the calibration, where batches pay, the race =="
./bin/u12_cost_bench
echo; echo "== 12.6  payoff: the sessions of Units 10 and 11, three ways =="
./bin/u12_session_bench
echo; echo "== the old queries: Unit 8's benchmark against Unit 11's build =="
sh scripts/u12_before_after.sh
