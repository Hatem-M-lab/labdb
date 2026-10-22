#!/bin/sh
# Challenge 9.1: Unit 8's own benchmark (tests/u08_sql_bench.cpp, unchanged
# since Unit 8), built twice -- against Unit 8's executor, checked out from the
# unit-08 tag, and against the current one -- and run five times each,
# alternating, so both builds see the same machine conditions. Prints the
# median of each measurement for each build.
#
# Needs a git clone of the repository (for the unit-08 tag). Takes ~2 minutes.
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
for i in 1 2 3 4 5; do
  measure "$old" >> "$tmp/old.txt"
  measure "$new" >> "$tmp/new.txt"
done

median() { sort -n | sed -n 3p; }   # the middle of five
col() { awk -v c="$2" '{ print $c }' "$1" | median; }

oi=$(col "$tmp/old.txt" 1); oq=$(col "$tmp/old.txt" 2); os=$(col "$tmp/old.txt" 3)
ni=$(col "$tmp/new.txt" 1); nq=$(col "$tmp/new.txt" 2); ns=$(col "$tmp/new.txt" 3)
echo "Unit 8's benchmark, 5 alternating runs of each build, medians:"
echo
printf '                                Unit 8 executor   Unit 9 operators\n'
printf '  INSERTs per second            %15s   %16s\n' "$oi" "$ni"
printf '  point lookups per second      %15s   %16s\n' "$oq" "$nq"
printf '  full-scan rows per second     %15s   %16s\n' "$os" "$ns"

git worktree remove --force "$tmp/u08"
rm -rf "$tmp"
