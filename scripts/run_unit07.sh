#!/bin/sh
# Reproduce every Unit 7 result from a clean build. Run from the repo root.
set -e
make all
echo; echo "== 7.2/7.3  parser: ASTs, round-trips, caret errors =="
./bin/u07_parser_test
echo; echo "== 7.4  forensic trap: the escaped-quote swallow =="
./bin/u07_escape_swallow_demo
echo; echo "== 7.5  payoff: a script parsed and round-tripped, measured =="
./bin/u07_parse_bench
