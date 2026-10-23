#!/bin/sh
# Unit 12: Unit 9's pipeline (the first table of tests/u12_pipeline_bench.cpp),
# counted in instructions instead of timed. Each plan runs under valgrind's
# callgrind once over the 1,000,000 rows and once not at all, after building
# the same rows; the difference divided by the rows is what one row costs.
# A few nanoseconds a row are within the noise of a shared machine; these
# counts are the same on every run. Needs valgrind. Takes about two minutes.
set -e
root=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
cd "$root"
make bin/u12_pipeline_bench >/dev/null
rows=1000000
per_row() {  # instructions one row costs in plan $1
  a=$(valgrind --tool=callgrind --callgrind-out-file=/dev/null ./bin/u12_pipeline_bench count "$1" 1 2>&1 |
      awk '/Collected/ { print $NF }')
  b=$(valgrind --tool=callgrind --callgrind-out-file=/dev/null ./bin/u12_pipeline_bench count "$1" 0 2>&1 |
      awk '/Collected/ { print $NF }')
  echo $(( (a - b) / rows ))
}
echo "Unit 9's pipeline, both ways, in instructions per row (valgrind --tool=callgrind, $rows rows)"
echo
printf '  %-62s %15s   %s\n' "" "a row at a time" "in batches of 1024"
for part in loop scan filter project; do
  case $part in
    loop) label="a hand-written loop over the data where it lies" ;;
    scan) label="the scan                        (1 operator)" ;;
    filter) label="Filter <- the scan              (2 operators)" ;;
    project) label="Project <- Filter <- the scan   (3 operators)" ;;
  esac
  printf '  %-62s %15d   %18d\n' "$label" "$(per_row rows-$part)" "$(per_row batches-$part)"
done
