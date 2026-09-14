#!/bin/sh
# Assemble the Unit 4 chapter from prose parts and captured program output.
# Run from the repository root. Code excerpts are inline in the prose parts
# (the buffer pool sources are large; the full files are in the tarball).
set -e
P=book/parts
OUT=book/04-unit-04-buffer-pool.md
: > "$OUT"

emit()       { cat "$1" >> "$OUT"; printf '\n' >> "$OUT"; }
after_open() { cat "$1" >> "$OUT"; printf '```\n\n' >> "$OUT"; }  # into open fence, then close

# ---- opener ----
emit  $P/p4_010_open.md

# ---- Challenge 4.1: frames, page table, pinning ----
emit  $P/p4_100_c41_head.md
emit  $P/p4_110_c41_why.md

# ---- Challenge 4.2: the clock replacement policy ----
emit  $P/p4_200_c42_head.md
emit  $P/p4_210_c42_why.md

# ---- Challenge 4.3: write-back + the pager-compatible surface ----
emit  $P/p4_300_c43_head.md
emit  $P/p4_310_c43_why.md

# ---- Challenge 4.4: the forensic trap (lost dirty page) ----
emit  $P/p4_400_trap_head.md
emit  $P/p4_410_trap_demorun.md          # opens ```text + command
after_open out/u04_lost_update_demo.txt
emit  $P/p4_420_trap_diag.md
emit  $P/p4_430_trap_testrun.md          # opens ```text + command
after_open out/u04_buffer_pool_test.txt
emit  $P/p4_440_trap_close.md

# ---- Challenge 4.5: the payoff, measured ----
emit  $P/p4_500_c45_head.md
emit  $P/p4_510_c45_run.md               # opens ```text + command
after_open out/u04_cache_bench.txt
emit  $P/p4_520_c45_interp.md

# ---- closer / appendix / manifest ----
emit  $P/p4_700_close.md

echo "wrote $OUT"
