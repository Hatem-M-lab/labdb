#!/bin/sh
# Build and run everything Unit 1 claims. From the repository root:
#   sh scripts/run_unit01.sh
set -e
make all
make test
make bench
./bin/u01_torn_write_demo torn.img
echo "--- hexdump of the torn page ---"
od -A d -t x1 torn.img
rm -f torn.img
