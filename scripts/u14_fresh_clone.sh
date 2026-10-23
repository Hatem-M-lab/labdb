#!/bin/sh
# Unit 14: the whole engine as a reader first meets it. Clones this
# repository into a temporary directory, checks out a tag (unit-14 unless one
# is given), and builds and tests it from nothing: make all, then make test.
# Prints what the build and the tests did, and fails if either does.
#
#   sh scripts/u14_fresh_clone.sh [TAG]
#
# Needs a git clone of the repository (for its tags). Takes ~5 minutes.
set -e
root=$(git rev-parse --show-toplevel)
tag=${1:-unit-14}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git clone -q "$root" "$tmp/labdb"
cd "$tmp/labdb"
git checkout -q "$tag"
echo "a fresh clone, at $tag ($(git rev-parse --short HEAD)): $(git ls-files | wc -l) files"
t0=$(date +%s)
if ! make -j2 all > build.log 2>&1; then tail -20 build.log; exit 1; fi
t1=$(date +%s)
echo "make all:  $(grep -c -E 'warning|error' build.log) warnings or errors; $(ls bin | wc -l) programs built, in $((t1 - t0)) s"
if ! make test > test.log 2>&1; then tail -20 test.log; exit 1; fi
t2=$(date +%s)
echo "make test: $(grep -c -E '^\./bin/|^sh scripts/' test.log) programs and scripts run, every one of them passed, in $((t2 - t1)) s"
echo "           the last of them:"
grep -E '^\./bin/|^sh scripts/' test.log | tail -6 | sed 's/^/             /'
