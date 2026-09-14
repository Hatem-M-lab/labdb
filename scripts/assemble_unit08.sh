#!/bin/sh
set -e
P=book/parts
OUT=book/08-unit-08-the-executor.md
: > "$OUT"
emit()       { cat "$1" >> "$OUT"; printf '\n' >> "$OUT"; }
after_open() { cat "$1" >> "$OUT"; printf '```\n\n' >> "$OUT"; }
emit  $P/p8_010_open.md
emit  $P/p8_100_c81_head.md
emit  $P/p8_200_c82_head.md
emit  $P/p8_300_c83_head.md              # opens ```text + sql_test
after_open out/u08_sql_test.txt
emit  $P/p8_320_c83_interp.md
emit  $P/p8_400_trap_head.md             # opens ```text + key_range_demo
after_open out/u08_key_range_demo.txt
emit  $P/p8_420_trap_diag.md
emit  $P/p8_500_c85_head.md              # opens ```text + sql_bench
after_open out/u08_sql_bench.txt
emit  $P/p8_520_c85_interp.md
emit  $P/p8_700_close.md
echo "wrote $OUT"
