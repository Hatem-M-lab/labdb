#!/bin/sh
# Unit 12, Challenge 12.2: what an invariant check that is never inlined costs
# a scan. tools/u12_row_probe.cpp is built twice -- against this engine, and
# against a copy whose check_that is written as it was through Unit 11, with
# the failure's message built inside the check itself -- and the instructions
# one row costs are counted under callgrind at three steps of tests/anatomy.hpp:
# a row decoded in place, a row of SeqScan's row path, a row of a batch.
# Needs valgrind. Takes about a minute.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
cp -r "$root/src" "$root/tests" "$root/tools" "$tmp/"
# the old check_that: one function that tests, and builds the message itself
python3 - "$tmp/src/common.hpp" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
start = s.index("// For internal invariants.")
end = s.index("// ---------------------------------------------------------------------------\n// Byte-exact")
s = s[:start] + '''// For internal invariants.
inline void check_that(bool ok, const char* what) {
  if (!ok) fail(std::string("invariant violated: ") + what);
}

''' + s[end:]
open(p, "w").write(s)
EOF
for side in old new; do
  if [ $side = old ]; then src="$tmp/src"; else src="$root/src"; fi
  files=""
  files=$(ls "$src"/*.cpp)  # every engine source
  g++ -std=c++20 -O2 -I"$src" -I"$root/tests" "$root/tools/u12_row_probe.cpp" $files -o "$tmp/probe_$side"
done
"$tmp/probe_new" build "$tmp/events.db"
n=20000
per_row() {  # probe step
  a=$(valgrind --tool=callgrind --callgrind-out-file=/dev/null "$1" "$2" $n "$tmp/events.db" 2>&1 | awk '/Collected/ { print $NF }')
  b=$(valgrind --tool=callgrind --callgrind-out-file=/dev/null "$1" "$2" 0 "$tmp/events.db" 2>&1 | awk '/Collected/ { print $NF }')
  echo $(( (a - b) / n ))
}
echo "instructions per row (valgrind --tool=callgrind, $n rows of events), with check_that"
echo
printf '  %-14s %16s %16s\n' "step" "as Unit 11 had it" "inlined (now)"
for step in decode_into scan_pin batches; do
  o=$(per_row "$tmp/probe_old" $step)
  w=$(per_row "$tmp/probe_new" $step)
  printf '  %-14s %16d %16d  (%+d)\n' "$step" "$o" "$w" $((w - o))
done
rm -rf "$tmp"
