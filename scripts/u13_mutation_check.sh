#!/bin/sh
# Unit 13: is the crash test good enough to catch a bug in the log? Seven bugs
# are planted, one at a time, in a copy of the engine, and
# tests/u13_crash_test.cpp is built and run against each copy (crashing at
# every 3rd write and sync): every one must fail. Then the unplanted copy must
# pass. Each bug is a line or two changed, the kind of slip a log invites:
#
#   1. a commit acknowledged before the log is synced
#   2. recovery that keeps the frames of a transaction the crash cut off
#   3. recovery that never checks a frame's checksum
#   4. a checkpoint that resets the log before the database file is synced
#   5. a rollback that leaves the transaction's pages in the pool
#   6. a log reset without a new generation (Challenge 13.5)
#   7. a checkpoint under kNormal that copies frames the log has not synced
#
# Takes ~6 minutes.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
missed=0

run() {  # build the copy in $tmp/w and run the test there
  ls "$tmp"/w/src/*.cpp | xargs -P 2 -I{} sh -c \
    'g++ -std=c++20 -O2 -I"$1/src" -I"$1/tests" -c "$2" -o "$1/obj_$(basename "$2" .cpp).o"' _ "$tmp/w" {}
  g++ -std=c++20 -O2 -I"$tmp/w/src" -I"$tmp/w/tests" "$tmp/w/tests/u13_crash_test.cpp" "$tmp"/w/obj_*.o \
    -o "$tmp/w/test"
  (cd "$tmp/w" && ulimit -v 4000000 && sh -c 'timeout 600 ./test 3 > out.txt 2>&1' 2>/dev/null)
}

plant() {  # name, file in src/, the text to replace, its replacement
  rm -rf "$tmp/w"
  mkdir -p "$tmp/w"
  cp -r "$root/src" "$root/tests" "$tmp/w/"
  if ! python3 - "$tmp/w/src/$2" "$3" "$4" <<'EOF'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(path).read()
if s.count(old) != 1:
    sys.exit(1)
open(path, "w").write(s.replace(old, new))
EOF
  then
    echo "  NOT PLANTED  $1 (the text was not found once)"
    missed=1
    return
  fi
  if run; then
    echo "  MISSED       $1"
    missed=1
  else
    echo "  caught       $1"
    why=$(grep -m1 -E 'WRONG|FAIL|the engine, after|invariant' "$tmp/w/out.txt" | sed "s|$tmp/w/||" | cut -c1-96)
    echo "               ${why:-(the test did not finish)}"
  fi
}

nl='
'
echo "seven planted bugs, each in its own copy of the engine, against u13_crash_test:"
echo
plant "1. a commit acknowledged before the log is synced" wal.cpp \
  "  if (sync) Wal::sync();" "  (void)sync;"
plant "2. a transaction the crash cut off, kept" wal.cpp \
  "  r.discarded = r.frames - commit_end_;" \
  "  for (const auto& [page, s] : pending_) committed_[page] = s;${nl}  r.discarded = r.frames - commit_end_;"
plant "3. a frame's checksum never checked" wal.cpp \
  "    if (load_u64(buf + 16) !=" "    if (false && load_u64(buf + 16) !="
plant "4. the log reset before the database file is synced" pager.cpp \
  "  if (durability_ != Durability::kOff) sync_exact(fd_);${nl}  file_pages_ = std::max(file_pages_, meta_.page_count);${nl}  wal_.reset();" \
  "  wal_.reset();${nl}  if (durability_ != Durability::kOff) sync_exact(fd_);${nl}  file_pages_ = std::max(file_pages_, meta_.page_count);"
plant "5. a rollback that leaves the transaction's pages in the pool" buffer_pool.cpp \
  "    check_that(f.pins == 0, \"rollback with a changed page still pinned\");${nl}    drop(fi);" \
  "    check_that(f.pins == 0, \"rollback with a changed page still pinned\");"
plant "6. a log reset without a new generation" wal.cpp \
  "  end_ = commit_end_ = 0;${nl}  if (!keep_generation_) ++generation_;${nl}  write_header();" \
  "  end_ = commit_end_ = 0;${nl}  write_header();"
plant "7. a checkpoint that copies frames the log has not synced" pager.cpp \
  "  if (durability_ == Durability::kNormal) wal_.sync();" "  (void)0;"

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
