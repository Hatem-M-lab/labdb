#!/bin/sh
# Assemble the Unit 1 chapter from prose parts, source listings, and
# captured program output. Run from the repository root.
set -e
P=book/parts
OUT=book/01-unit-01-slotted-pages-and-the-pager.md
: > "$OUT"

emit()      { cat "$1" >> "$OUT"; printf '\n' >> "$OUT"; }
code()      { printf '```cpp\n'   >> "$OUT"; cat "$1" >> "$OUT"; printf '```\n\n' >> "$OUT"; }
close()     { printf '```\n\n'    >> "$OUT"; }              # close an already-open fence
out_txt()   { cat "$1" >> "$OUT"; }                          # raw: file already holds a md table
after_open(){ cat "$1" >> "$OUT"; printf '```\n\n' >> "$OUT"; }  # append into open fence, then close

# ---- opener ----
emit  $P/p010_open.md

# ---- Challenge 1.1 ----
emit  $P/p100_c11_head.md
code  src/common.hpp
emit  $P/p101_c11_mid.md
code  src/page.hpp
emit  $P/p102_c11_why.md          # ends: open ```text + "$ ./bin/u01_page_layout_test"
after_open out/u01_page_layout_test.txt
emit  $P/p103_c11_bench.md
code  tests/u01_header_bench.cpp
emit  $P/p104_c11_run.md          # ends: open ```text + "$ ./bin/u01_header_bench"
after_open out/u01_header_bench.txt
emit  $P/p105_c11_asm.md
code  tools/u01_header_asm.cpp
emit  $P/p106_c11_asmrun.md       # ends: open ```text + the g++ command line
after_open out/u01_header_asm.txt
emit  $P/p107_c11_close.md

# ---- Challenge 1.2 ----
emit  $P/p200_c12_head.md
code  snapshots/c12/slotted_page.hpp
emit  $P/p201_c12_why.md          # self-contained (table inline)

# ---- Challenge 1.3 ----
emit  $P/p300_c13_head.md
code  src/slotted_page.hpp
emit  $P/p301_c13_why.md
code  tests/u01_slotted_test.cpp
emit  $P/p302_c13_testrun.md      # ends: open ```text + "$ ./bin/u01_slotted_test"
out_txt out/u01_slotted_test.txt  # raw table body...
close                             # ...then close the fence
emit  $P/p303_c13_churn_head.md
code  tests/u01_churn_bench.cpp
emit  $P/p304_c13_churnrun.md     # ends: open ```text + "$ ./bin/u01_churn_bench"
after_open out/u01_churn_bench.txt
emit  $P/p305_c13_close.md

# ---- Challenge 1.4 ----
emit  $P/p400_c14_head.md
code  src/io.hpp
code  src/io.cpp
emit  $P/p401_c14_pager.md
code  snapshots/c14/pager.hpp
code  snapshots/c14/pager.cpp
emit  $P/p402_c14_why.md
code  tests/u01_fsync_bench.cpp
emit  $P/p403_c14_fsyncrun.md     # ends: open ```text + "$ ./bin/u01_fsync_bench"
after_open out/u01_fsync_bench.txt
emit  $P/p404_c14_fsync_interp.md
# forensic trap
emit  $P/p410_trap_head.md
code  tools/u01_torn_write_demo.cpp
emit  $P/p411_trap_demorun.md     # self-contained (demo + hexdump inline)
code  tests/u01_torn_write_test.cpp
emit  $P/p412_trap_testrun.md     # self-contained (output inline)

# ---- Challenge 1.5 ----
emit  $P/p500_c15_head.md
code  src/pager.hpp
code  src/pager.cpp
emit  $P/p501_c15_why.md
code  tests/u01_pager_test.cpp
emit  $P/p502_c15_run.md          # ends: open ```text + "$ ./bin/u01_pager_test"
after_open out/u01_pager_test.txt
emit  $P/p503_c15_close.md

# ---- Challenge 1.6 ----
emit  $P/p600_c16_head.md
code  tests/u01_scan_bench.cpp
emit  $P/p601_c16_run.md          # ends: open ```text + "$ ./bin/u01_scan_bench"
after_open out/u01_scan_bench.txt
emit  $P/p602_c16_interp.md       # self-contained (filefrag inline)

# ---- closer / appendix ----
emit  $P/p700_close.md

echo "wrote $OUT"
