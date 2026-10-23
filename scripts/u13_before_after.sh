#!/bin/sh
# Unit 13: what does the log do to the old queries? Unit 8's own benchmark
# (tests/u08_sql_bench.cpp, unchanged since Unit 8), built twice -- against
# Unit 12's engine, checked out from the unit-12 tag, and against this
# checkout's -- and run RUNS times each (default 3), alternating, so both
# builds see the same machine conditions. Its INSERTs are one transaction
# each, so here every one of them waits for an fsync. Prints the median of
# each measurement for each build, and the range (lowest-highest) it moved
# within over those runs: a difference smaller than that range is noise, not
# a result.
#
# Then, if valgrind is installed, the instructions each build executes per
# point lookup, stage by stage, and per INSERT, which do not move from run to
# run.
#
# Needs a git clone of the repository (for the unit-12 tag). Takes ~4 minutes.
set -e
root=$(git rev-parse --show-toplevel)
cd "$root"
make bin/u08_sql_bench >/dev/null
tmp=$(mktemp -d)
git worktree add --detach "$tmp/base" unit-12 >/dev/null 2>&1
make -C "$tmp/base" bin/u08_sql_bench >/dev/null
old="$tmp/base/bin/u08_sql_bench"
new="$root/bin/u08_sql_bench"

# One run: print "inserts/s lookups/s scan-rows/s".
measure() {
  (cd "$tmp" && "$1") | awk '
    /INSERTs\/s/       { for (i = 1; i <= NF; i++) if ($i == "INSERTs/s).") { v = $(i-1); gsub(/[(]/, "", v); ins = v } }
    /queries\/s/       { for (i = 1; i <= NF; i++) if ($i == "queries/s") q = $(i-1) }
    /rows scanned\/s/  { for (i = 1; i <= NF; i++) if ($i == "rows" && $(i+1) == "scanned/s") s = $(i-1) }
    END                { print ins, q, s }'
}

: > "$tmp/old.txt"
: > "$tmp/new.txt"
RUNS=${RUNS:-3}
i=0
while [ "$i" -lt "$RUNS" ]; do
  measure "$old" >> "$tmp/old.txt"
  measure "$new" >> "$tmp/new.txt"
  i=$((i + 1))
done

# "median (lowest-highest)" of column $2 of file $1
stat() {
  awk -v c="$2" '{ print $c }' "$1" | sort -n | awk '
    { v[NR] = $1 }
    END { printf "%d (%d-%d)", v[int((NR + 1) / 2)], v[1], v[NR] }'
}

echo "Unit 8's benchmark, $RUNS alternating runs of each build: median (lowest-highest)"
echo
printf '                         %-28s %s\n' "Unit 12 (tag unit-12)" "this checkout"
printf '  INSERTs/s              %-28s %s\n' "$(stat "$tmp/old.txt" 1)" "$(stat "$tmp/new.txt" 1)"
printf '  point lookups/s        %-28s %s\n' "$(stat "$tmp/old.txt" 2)" "$(stat "$tmp/new.txt" 2)"
printf '  full-scan rows/s       %-28s %s\n' "$(stat "$tmp/old.txt" 3)" "$(stat "$tmp/new.txt" 3)"

# Instructions per point lookup, by stage -- exact, where the times above
# move by a fifth from run to run. Needs valgrind; skipped without it. The
# probe (tools/u10_query_probe.cpp) is built against each tree's engine.
if command -v valgrind >/dev/null 2>&1; then
  for side in base root; do
    if [ "$side" = base ]; then src="$tmp/base"; else src="$root"; fi
    files=""
    files=$(ls "$src"/src/*.cpp)  # every engine source, whichever unit the tree is at
    g++ -std=c++20 -O2 -I"$src/src" -I"$src/tests" "$root/tools/u10_query_probe.cpp" $files \
      -o "$tmp/probe_$side"
  done
  count() {  # instructions for one run of: probe STAGE N
    (cd "$tmp" && valgrind --tool=callgrind --callgrind-out-file=/dev/null "$1" "$2" "$3" 2>&1) |
      awk '/Collected/ { print $NF }'
  }
  n=20000
  echo
  echo "instructions per point lookup, by stage, and per INSERT (valgrind --tool=callgrind, $n each):"
  echo
  printf '                         %-28s %s\n' "Unit 12 (tag unit-12)" "this checkout"
  for stage in parse plan query insert; do
    b=$(( ($(count "$tmp/probe_base" $stage $n) - $(count "$tmp/probe_base" $stage 0)) / n ))
    c=$(( ($(count "$tmp/probe_root" $stage $n) - $(count "$tmp/probe_root" $stage 0)) / n ))
    printf '  %-22s %-28s %s (%+d)\n' "$stage" "$b" "$c" $((c - b))
  done
fi

git worktree remove --force "$tmp/base"
rm -rf "$tmp"
