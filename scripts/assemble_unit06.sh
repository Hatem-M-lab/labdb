#!/bin/sh
# Assemble the Unit 6 chapter from prose parts and captured program output.
# Run from the repository root.
set -e
P=book/parts
OUT=book/06-unit-06-the-catalog.md
: > "$OUT"
emit()       { cat "$1" >> "$OUT"; printf '\n' >> "$OUT"; }
after_open() { cat "$1" >> "$OUT"; printf '```\n\n' >> "$OUT"; }

emit  $P/p6_010_open.md
emit  $P/p6_100_c61_head.md

emit  $P/p6_200_c62_head.md              # opens ```text + catalog_test command
after_open out/u06_catalog_test.txt
emit  $P/p6_220_c62_interp.md

emit  $P/p6_300_c63_head.md

emit  $P/p6_400_trap_head.md             # opens ```text + prefix_collision_demo command
after_open out/u06_prefix_collision_demo.txt
emit  $P/p6_420_trap_diag.md

emit  $P/p6_500_c65_head.md              # opens ```text + catalog_bench command
after_open out/u06_catalog_bench.txt
emit  $P/p6_520_c65_interp.md

emit  $P/p6_700_close.md
echo "wrote $OUT"
