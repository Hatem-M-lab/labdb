#!/bin/sh
# Challenge 9.1: Unit 8's own benchmark (tests/u08_sql_bench.cpp, unchanged
# since Unit 8), built twice -- against Unit 8's executor, checked out from the
# unit-08 tag, and against the current one -- and run RUNS times each
# (default 9), alternating, so both builds see the same machine conditions.
# Prints the median of each measurement for each build, and the range
# (lowest-highest) it moved within over those runs: a difference smaller than
# that range is noise, not a result.
#
# Needs a git clone of the repository (for the unit-08 tag). Takes ~4 minutes.
set -e
root=$(git rev-parse --show-toplevel)
cd "$root"
make bin/u08_sql_bench >/dev/null
tmp=$(mktemp -d)
git worktree add --detach "$tmp/u08" unit-08 >/dev/null 2>&1
make -C "$tmp/u08" bin/u08_sql_bench >/dev/null
old="$tmp/u08/bin/u08_sql_bench"
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
RUNS=${RUNS:-9}
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
printf '                         %-28s %s\n' "Unit 8 executor" "Unit 9 operators"
printf '  INSERTs/s              %-28s %s\n' "$(stat "$tmp/old.txt" 1)" "$(stat "$tmp/new.txt" 1)"
printf '  point lookups/s        %-28s %s\n' "$(stat "$tmp/old.txt" 2)" "$(stat "$tmp/new.txt" 2)"
printf '  full-scan rows/s       %-28s %s\n' "$(stat "$tmp/old.txt" 3)" "$(stat "$tmp/new.txt" 3)"

git worktree remove --force "$tmp/u08"
rm -rf "$tmp"
