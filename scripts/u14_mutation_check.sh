#!/bin/sh
# Unit 14: is the comparison with PostgreSQL good enough to catch a wrong
# answer? Eight bugs are planted, one at a time, in a copy of the engine, and
# tests/u14_pg_diff.cpp is built against each copy and run (3,000 queries)
# through scripts/u14_postgres_check.sh: every one must fail. Then the
# unplanted copy must pass. Each bug is a line changed, the kind of slip a
# query engine invites:
#
#   1. NULL sorted last in both directions, in the batch path
#   2. the same, in the row path
#   3. text compared as signed chars
#   4. an index key built from signed chars
#   5. MIN and MAX of text taken from the first index entry, like a number's
#      (run twice: first without the word that ties at the top, which the
#      comparison's first version did not have and then misses -- and then
#      with it)
#   6. COUNT(column) counting NULLs, in the batch path
#   7. AVG by integer division, in the batch path
#   8. an index join that forgets its other ON terms
#
# Needs PostgreSQL, as scripts/u14_postgres_check.sh does. Takes ~5 minutes.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
if ! command -v psql >/dev/null 2>&1; then
  echo "PostgreSQL (psql) not found: skipped"
  exit 0
fi
tmp=$(mktemp -d)
missed=0

run() {  # build the copy in $tmp/w and run the comparison against it ($1: --no-tie, or nothing)
  ls "$tmp"/w/src/*.cpp | xargs -P 2 -I{} sh -c \
    'g++ -std=c++20 -O2 -I"$1/src" -I"$1/tests" -c "$2" -o "$1/obj_$(basename "$2" .cpp).o"' _ "$tmp/w" {}
  g++ -std=c++20 -O2 -I"$tmp/w/src" -I"$tmp/w/tests" "$tmp/w/tests/u14_pg_diff.cpp" "$tmp"/w/obj_*.o \
    -o "$tmp/w/pg_diff"
  PG_DIFF="$tmp/w/pg_diff" sh "$root/scripts/u14_postgres_check.sh" 3000 $1 > "$tmp/w/out.txt" 2>&1
}

plant() {  # name, file in src/, the text to replace, its replacement[, --no-tie]
  rm -rf "$tmp/w"
  mkdir -p "$tmp/w"
  cp -r "$root/src" "$root/tests" "$tmp/w/"
  if ! python3 - "$tmp/w/src/$2" "$3" "$4" <<'PY'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(path).read()
if s.count(old) != 1:
    sys.exit(1)
open(path, "w").write(s.replace(old, new))
PY
  then
    echo "  NOT PLANTED  $1 (the text was not found once)"
    missed=1
    return
  fi
  if run "$5"; then
    if [ -n "$5" ]; then
      echo "  missed       $1, without the word that ties at the top -- as expected"
    else
      echo "  MISSED       $1"
      missed=1
    fi
  else
    echo "  caught       $1"
    n=$(grep -m1 'different  ' "$tmp/w/out.txt" | awk '{ print $2 }')
    q=$(grep -m1 'DIFFERENT:' "$tmp/w/out.txt" | sed 's/^ *DIFFERENT: //' | cut -c1-84)
    echo "               ${n:-?} of 3000 different; the first: ${q:-(none shown)}"
  fi
}

echo "eight planted bugs, each in its own copy of the engine, against u14_pg_diff:"
echo
plant "1. NULL last in both directions (batches)" operator.cpp \
  "      return k.desc ? pn : qn;" "      return qn;"
plant "2. NULL last in both directions (rows)" operator.cpp \
  "      return k.desc ? p.is_null : q.is_null;" "      return q.is_null;"
plant "3. text compared as signed chars" operator.cpp \
  "    const int c = a.text.compare(b.text);" \
  "    const int c = std::lexicographical_compare(a.text.begin(), a.text.end(), b.text.begin(), b.text.end()) ? -1 : a.text == b.text ? 0 : 1;"
plant "4. an index key built from signed chars" record.hpp \
  "    p |= std::uint64_t{static_cast<unsigned char>(t[i])} << (56 - 8 * i);" \
  "    p |= static_cast<std::uint64_t>(t[i]) << (56 - 8 * i);"
plant "5. MIN and MAX of text from the first index entry" operator.cpp \
  "        if (order_key_exact(a.type)) break;  // an exact key is the value: one row is enough" \
  "        break;" --no-tie
plant "5. MIN and MAX of text from the first index entry" operator.cpp \
  "        if (order_key_exact(a.type)) break;  // an exact key is the value: one row is enough" \
  "        break;"
plant "6. COUNT(column) counting NULLs (batches)" operator.cpp \
  "cnt[gid[k]] += null[i] ? 0 : 1;" "cnt[gid[k]] += 1;"
plant "7. AVG by integer division (batches)" operator.cpp \
  "dst.doubles[j] = count ? static_cast<double>(sums_[a][g]) / static_cast<double>(count) : 0.0;" \
  "dst.doubles[j] = count ? static_cast<double>(sums_[a][g] / static_cast<std::int64_t>(count)) : 0.0;"
plant "8. an index join that forgets its other ON terms" executor.cpp \
  "table_terms(i), residual_without(pick->key)," "table_terms(i), std::vector<BoundComparison>{},"

rm -rf "$tmp/w"
mkdir -p "$tmp/w"
cp -r "$root/src" "$root/tests" "$tmp/w/"
echo
if run; then
  echo "the engine as it is: $(grep -m1 'different  ' "$tmp/w/out.txt" | awk '{ print $2 }') of 3000 different," \
       "$(tail -1 "$tmp/w/out.txt")"
else
  echo "the engine as it is: FAILED"
  missed=1
fi
rm -rf "$tmp"
exit $missed
