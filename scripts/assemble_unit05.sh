#!/bin/sh
# Assemble the Unit 5 chapter from prose parts and captured program output.
# Run from the repository root. Code excerpts are inline in the prose parts;
# the full sources are in the tarball.
set -e
P=book/parts
OUT=book/05-unit-05-the-record-layer.md
: > "$OUT"

emit()       { cat "$1" >> "$OUT"; printf '\n' >> "$OUT"; }
after_open() { cat "$1" >> "$OUT"; printf '```\n\n' >> "$OUT"; }  # into open fence, then close

# ---- opener ----
emit  $P/p5_010_open.md

# ---- Challenge 5.1: schema + fixed-width record + null bitmap ----
emit  $P/p5_100_c51_head.md
emit  $P/p5_110_c51_why.md

# ---- Challenge 5.2: variable-length text + projection ----
emit  $P/p5_200_c52_head.md
emit  $P/p5_210_c52_why.md               # opens ```text + command
after_open out/u05_record_test.txt
emit  $P/p5_220_c52_interp.md

# ---- Challenge 5.3: the table heap ----
emit  $P/p5_300_c53_head.md
emit  $P/p5_310_c53_why.md               # opens ```text + command
after_open out/u05_heap_test.txt
emit  $P/p5_320_c53_interp.md

# ---- Challenge 5.4: the forensic trap (null-slot shift) ----
emit  $P/p5_400_trap_head.md             # opens ```text + command
after_open out/u05_null_shift_demo.txt
emit  $P/p5_420_trap_diag.md
emit  $P/p5_440_trap_close.md

# ---- Challenge 5.5: the payoff, measured ----
emit  $P/p5_500_c55_head.md              # opens ```text + command
after_open out/u05_end_to_end_bench.txt
emit  $P/p5_520_c55_interp.md

# ---- closer / manifest / appendix ----
emit  $P/p5_700_close.md

echo "wrote $OUT"
