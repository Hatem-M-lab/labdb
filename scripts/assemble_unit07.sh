#!/bin/sh
# Assemble the Unit 7 chapter from prose parts and captured program output.
set -e
P=book/parts
OUT=book/07-unit-07-the-front-end.md
: > "$OUT"
emit()       { cat "$1" >> "$OUT"; printf '\n' >> "$OUT"; }
after_open() { cat "$1" >> "$OUT"; printf '```\n\n' >> "$OUT"; }

emit  $P/p7_010_open.md
emit  $P/p7_100_c71_head.md
emit  $P/p7_200_c72_head.md              # opens ```text + parser_test command
after_open out/u07_parser_test.txt
emit  $P/p7_220_c72_interp.md
emit  $P/p7_300_c73_head.md
emit  $P/p7_400_trap_head.md             # opens ```text + escape_swallow_demo command
after_open out/u07_escape_swallow_demo.txt
emit  $P/p7_420_trap_diag.md
emit  $P/p7_500_c75_head.md              # opens ```text + parse_bench command
after_open out/u07_parse_bench.txt
emit  $P/p7_520_c75_interp.md
emit  $P/p7_700_close.md
echo "wrote $OUT"
