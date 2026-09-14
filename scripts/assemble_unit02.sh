#!/bin/sh
# Assemble the Unit 2 chapter from prose parts, the two B+Tree source
# listings, and captured program output. Run from the repository root.
set -e
P=book/parts
OUT=book/02-unit-02-btree-insert-and-search.md
: > "$OUT"

emit()       { cat "$1" >> "$OUT"; printf '\n' >> "$OUT"; }
code()       { printf '```cpp\n' >> "$OUT"; cat "$1" >> "$OUT"; printf '```\n\n' >> "$OUT"; }
after_open() { cat "$1" >> "$OUT"; printf '```\n\n' >> "$OUT"; }  # append into open fence, then close

# ---- opener ----
emit  $P/p2_010_open.md

# ---- Challenge 2.1: the leaf (full btree.hpp listing here) ----
emit  $P/p2_100_c21_head.md
code  src/btree.hpp
emit  $P/p2_110_c21_why.md

# ---- Challenge 2.2: internal node + full search (inline excerpts) ----
emit  $P/p2_200_c22_head.md
emit  $P/p2_210_c22_why.md

# ---- Challenge 2.3: insert into a leaf ----
emit  $P/p2_300_c23_head.md

# ---- Challenge 2.4: leaf split + forensic trap ----
emit  $P/p2_400_c24_head.md
emit  $P/p2_410_c24_why.md
emit  $P/p2_420_trap_head.md
emit  $P/p2_430_trap_demorun.md          # opens ```text + command
after_open out/u02_median_bug_demo.txt   # ...output, then close
emit  $P/p2_440_trap_diag.md
emit  $P/p2_450_trap_testrun.md          # opens ```text + command
after_open out/u02_split_regression_test.txt
emit  $P/p2_460_trap_close.md

# ---- Challenge 2.5: internal split (full btree.cpp listing here) ----
emit  $P/p2_500_c25_head.md
code  src/btree.cpp
emit  $P/p2_510_c25_why.md
emit  $P/p2_520_c25_testrun.md           # opens ```text + command
after_open out/u02_btree_test.txt
emit  $P/p2_530_c25_close.md

# ---- Challenge 2.6: the payoff ----
emit  $P/p2_600_c26_head.md              # opens ```text + command
after_open out/u02_search_bench.txt
emit  $P/p2_610_c26_interp1.md           # opens ```text + command
after_open out/u02_insert_bench.txt
emit  $P/p2_620_c26_interp2.md

# ---- closer / appendix / manifest ----
emit  $P/p2_700_close.md

echo "wrote $OUT"
