#!/bin/sh
# Build and run everything Unit 4 claims. From the repository root:
#   sh scripts/run_unit04.sh
set -e
make all

echo "=== correctness: the buffer pool itself ==="
./bin/u04_buffer_pool_test

echo
echo "=== the forensic trap: the lost dirty page ==="
./bin/u04_lost_update_demo

echo
echo "=== the payoff: logical reads stay flat, disk reads collapse as the pool grows ==="
./bin/u04_cache_bench
