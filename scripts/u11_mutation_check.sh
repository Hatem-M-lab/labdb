#!/bin/sh
# Unit 11: is the differential test good enough to catch a planner bug? Five
# bugs are planted, one at a time, in a copy of the engine, and
# tests/u11_index_test.cpp is built and run against each copy: every one must
# fail. Then the unplanted copy must pass. Each bug is one line changed, the
# kind of slip the code invites:
#
#   1. the search descends by Unit 2's rule in a non-unique tree too
#   2. a TEXT range consumes its terms, as if its prefix key were exact
#   3. the index join forgets that a TEXT key is only a prefix
#   4. an exclusive lower bound is not stepped past
#   5. a comparison with NULL is ignored instead of emptying the range
#
# Takes ~2 minutes (six builds).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
engine="io pager btree buffer_pool heap catalog parser executor operator"
missed=0

run() {  # name: build the copy in $tmp/w and run the test there
  files=""
  for f in $engine; do files="$files $tmp/w/src/$f.cpp"; done
  g++ -std=c++20 -O2 -I"$tmp/w/src" -I"$tmp/w/tests" "$tmp/w/tests/u11_index_test.cpp" $files -o "$tmp/w/test"
  # (a planted bug ends the test with abort(); the inner shell's report of it is dropped)
  (cd "$tmp/w" && sh -c './test > out.txt 2>&1' 2>/dev/null)
}

plant() {  # name, file in src/, sed expression
  rm -rf "$tmp/w"
  mkdir -p "$tmp/w"
  cp -r "$root/src" "$root/tests" "$tmp/w/"
  sed -i "$3" "$tmp/w/src/$2"
  if cmp -s "$root/src/$2" "$tmp/w/src/$2"; then
    echo "  NOT PLANTED  $1 (the line was not found)"
    missed=1
    return
  fi
  if run; then
    echo "  MISSED       $1"
    missed=1
  else
    echo "  caught       $1"
    echo "               $(grep -m1 -E 'FAIL|MISMATCH' "$tmp/w/out.txt" | sed "s|$tmp/w/||" | cut -c1-96)"
  fi
}

echo "five planted bugs, each in its own copy of the engine, against u11_index_test:"
echo
plant "1. the search descends by Unit 2's rule" btree.cpp \
  's/return seek(lo, unique_ ? rightmost_child : leftmost_child);/return seek(lo, rightmost_child);/'
plant "2. a TEXT range consumes its terms" executor.cpp \
  's/consumes = order_key_exact(schema\[k.col\].column.type);/consumes = true;/'
plant "3. the index join skips its string comparison" operator.cpp \
  '/if (text_key_ \&\& inner_row_\[inner_col_\].text != outer_row_\[outer_col_\].text) continue;/d'
plant "4. an exclusive lower bound is not stepped past" executor.cpp \
  '/^      ++lo;$/d'
plant "5. a NULL literal leaves the range as it was" executor.cpp \
  's/if (b.literal.is_null) range.empty = true;/if (b.literal.is_null) (void)0;/'

rm -rf "$tmp/w"
mkdir -p "$tmp/w"
cp -r "$root/src" "$root/tests" "$tmp/w/"
echo
if run; then
  echo "the engine as it is: $(tail -1 "$tmp/w/out.txt")"
else
  echo "the engine as it is: FAILED"
  missed=1
fi
rm -rf "$tmp"
exit $missed
