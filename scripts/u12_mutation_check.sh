#!/bin/sh
# Unit 12: is the differential test good enough to catch a bug in the batch
# path? Seven bugs are planted, one at a time, in a copy of the engine, and
# tests/u12_batch_test.cpp is built and run against each copy: every one must
# fail. Then the unplanted copy must pass. Each bug is one line changed, the
# kind of slip batches invite:
#
#   1. a Filter that reports an emptied batch as the end (Challenge 12.5)
#   2. a term that reads every row of the batch, not just those still live
#   3. a probe that abandons a bucket the full output interrupted
#   4. COUNT(col) that counts NULLs too
#   5. a top n that turns a tied prefix away without comparing the rest
#   6. a gathered value that loses its NULL
#   7. a LIMIT that does not tell its input how many rows it still wants
#
# Each copy runs with a time and a memory limit, so that a bug which loops
# cannot take the machine down with it. Takes ~4 minutes.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
engine="io pager btree buffer_pool heap catalog parser executor operator"
missed=0

run() {  # build the copy in $tmp/w and run the test there
  files=""
  for f in $engine; do files="$files $tmp/w/src/$f.cpp"; done
  g++ -std=c++20 -O2 -I"$tmp/w/src" -I"$tmp/w/tests" "$tmp/w/tests/u12_batch_test.cpp" $files -o "$tmp/w/test"
  # (a planted bug ends the test with abort(); the inner shell's report of it is dropped)
  (cd "$tmp/w" && ulimit -v 4000000 && sh -c 'timeout 300 ./test > out.txt 2>&1' 2>/dev/null)
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
    why=$(grep -m1 -E 'FAIL|disagrees|another plan|invariant' "$tmp/w/out.txt" | sed "s|$tmp/w/||" | cut -c1-96)
    echo "               ${why:-(the test did not finish)}"
  fi
}

echo "seven planted bugs, each in its own copy of the engine, against u12_batch_test:"
echo
plant "1. an emptied batch reported as the end" operator.cpp \
  's/^    if (out.count() > 0) return true;$/    return out.count() > 0;/'
plant "2. a term that reads rows already dropped" operator.cpp \
  's/^  if (b.selected) {$/  if (false) {/'
plant "3. a bucket the full output interrupted, abandoned" operator.cpp \
  's/^      if (pos_ >= end_) {$/      if (true) {/'
plant "4. COUNT(col) counts NULLs" operator.cpp \
  's/cnt\[gid\[k\]\] += null\[i\] ? 0 : 1;/cnt[gid[k]] += 1;/'
plant "5. a tied prefix turned away from the top n" operator.cpp \
  's/if (p > order_.front().prefix) continue;/if (p >= order_.front().prefix) continue;/'
plant "6. a gathered value loses its NULL" operator.cpp \
  's/for (std::size_t j = 0; j < m; ++j) dn\[j\] = sn\[idx\[j\]\];/for (std::size_t j = 0; j < m; ++j) dn[j] = 0;/'
plant "7. a LIMIT that does not pass its budget down" operator.hpp \
  's/if (!child_->next_batch(out, static_cast<std::size_t>(want))) return false;/if (!child_->next_batch(out)) return false;/'

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
