#!/bin/sh
# Assemble the Unit 3 chapter from prose parts and captured program output.
# Run from the repository root. Code excerpts are inline in the prose parts
# (the btree files are large now; the full sources are in the tarball).
set -e
P=book/parts
OUT=book/03-unit-03-btree-delete-and-range-scans.md
: > "$OUT"

emit()       { cat "$1" >> "$OUT"; printf '\n' >> "$OUT"; }
after_open() { cat "$1" >> "$OUT"; printf '```\n\n' >> "$OUT"; }  # into open fence, then close

# ---- opener ----
emit  $P/p3_010_open.md

# ---- Challenge 3.1: the cursor / range scans ----
emit  $P/p3_100_c31_head.md
emit  $P/p3_110_c31_why.md

# ---- Challenge 3.2: deleting from a leaf ----
emit  $P/p3_200_c32_head.md

# ---- Challenge 3.3: borrowing ----
emit  $P/p3_300_c33_head.md
emit  $P/p3_310_c33_why.md

# ---- Challenge 3.4: merge + root collapse + forensic trap ----
emit  $P/p3_400_c34_head.md
emit  $P/p3_410_c34_why.md
emit  $P/p3_420_trap_head.md
emit  $P/p3_430_trap_demorun.md          # opens ```text + command
after_open out/u03_merge_chain_bug_demo.txt
emit  $P/p3_440_trap_diag.md
emit  $P/p3_450_trap_testrun.md          # opens ```text + command
after_open out/u03_scan_regression_test.txt
emit  $P/p3_460_trap_close.md

# ---- Challenge 3.5: the payoff, measured ----
emit  $P/p3_500_c35_head.md              # opens ```text + command
after_open out/u03_range_bench.txt
emit  $P/p3_510_c35_interp1.md           # opens ```text + command
after_open out/u03_delete_bench.txt
emit  $P/p3_520_c35_interp2.md

# ---- closer / appendix / manifest ----
emit  $P/p3_700_close.md

echo "wrote $OUT"
