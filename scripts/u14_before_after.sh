#!/bin/sh
# Unit 14's trap, against the engine before this unit: Unit 13's, checked out
# from the unit-13 tag. Builds tools/u14_literal_probe.cpp against Unit 13's
# engine and against this checkout's, and runs both. Then, if PostgreSQL is
# installed, builds the comparison with PostgreSQL (tests/u14_pg_diff.cpp)
# against Unit 13's engine and runs it, through scripts/u14_postgres_check.sh.
#
# Needs a git clone of the repository (for the unit-13 tag).
set -e
root=$(git rev-parse --show-toplevel)
cd "$root"
make bin/u14_literal_probe >/dev/null
tmp=$(mktemp -d)
trap 'git worktree remove --force "$tmp/base" >/dev/null 2>&1; rm -rf "$tmp"' EXIT
git worktree add --detach "$tmp/base" unit-13 >/dev/null 2>&1
flags="-std=c++20 -O2"
mkdir "$tmp/obj"
for f in "$tmp"/base/src/*.cpp; do
  g++ $flags -I"$tmp/base/src" -c "$f" -o "$tmp/obj/$(basename "$f" .cpp).o"
done
g++ $flags -I"$tmp/base/src" -I"$root/tests" tools/u14_literal_probe.cpp "$tmp"/obj/*.o -o "$tmp/probe13"

echo "Unit 13's engine (tag unit-13):"
(cd "$tmp" && ./probe13)
echo
echo "this checkout's engine:"
(cd "$tmp" && "$root/bin/u14_literal_probe")

if command -v psql >/dev/null 2>&1; then
  g++ $flags -I"$tmp/base/src" -I"$root/tests" tests/u14_pg_diff.cpp "$tmp"/obj/*.o -o "$tmp/pg_diff13"
  echo
  echo "the comparison with PostgreSQL, against Unit 13's engine:"
  PG_DIFF="$tmp/pg_diff13" sh scripts/u14_postgres_check.sh 1000 || true
fi
