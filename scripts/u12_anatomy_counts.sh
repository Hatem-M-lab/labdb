#!/bin/sh
# Unit 12: the steps of a scanned row (tests/anatomy.hpp), counted in
# instructions instead of timed. Each step runs under valgrind's callgrind
# twice, over N rows and over none, after the same warm-up; the difference
# divided by N is what one row costs at that step. Unlike the times of
# u12_anatomy_bench, these counts are the same on every run. Needs valgrind.
# Takes about a minute.
set -e
root=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
cd "$root"
make bin/u12_row_probe >/dev/null
tmp=$(mktemp -d)
db="$tmp/events.db"
./bin/u12_row_probe build "$db"
n=${N:-20000}
per_row() {  # instructions one row costs at step $1
  a=$(valgrind --tool=callgrind --callgrind-out-file=/dev/null ./bin/u12_row_probe "$1" "$n" "$db" 2>&1 |
      awk '/Collected/ { print $NF }')
  b=$(valgrind --tool=callgrind --callgrind-out-file=/dev/null ./bin/u12_row_probe "$1" 0 "$db" 2>&1 |
      awk '/Collected/ { print $NF }')
  echo $(( (a - b) / n ))
}
echo "instructions per row, step by step (valgrind --tool=callgrind, $n rows of events)"
for group in "walk zero_page copy_page copy_record decode scan_copy" "walk pin decode_into scan_pin" "batches"; do
  echo
  case $group in
    walk\ zero*) echo "  as Unit 11 reads it" ;;
    walk\ pin*) echo "  as Unit 12 reads it" ;;
    *) echo "  in batches of 1,024 rows" ;;
  esac
  prev=""
  for step in $group; do
    c=$(per_row "$step")
    if [ -z "$prev" ]; then printf '  %-12s %6d\n' "$step" "$c"
    else printf '  %-12s %6d  (%+d)\n' "$step" "$c" $((c - prev)); fi
    prev=$c
  done
done
rm -rf "$tmp"
