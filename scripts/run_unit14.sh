#!/bin/sh
set -e
make all
echo; echo "== 14.1  the engine as a program: a session, and kill -9 =="
sh scripts/u14_shell_check.sh
echo; echo "== 14.2  labdb against PostgreSQL: 20,000 statements =="
sh scripts/u14_postgres_check.sh 20000
echo; echo "== 14.3  the comparison, tested: eight planted bugs =="
sh scripts/u14_mutation_check.sh
echo; echo "== 14.4  labdb against SQLite =="
if [ -x bin/u14_sqlite_bench ]; then ./bin/u14_sqlite_bench; else echo "SQLite (libsqlite3-dev) not found: skipped"; fi
echo; echo "== 14.5  forensic trap: the numbers the engine could not write =="
./bin/u14_literal_probe --check
sh scripts/u14_before_after.sh
echo; echo "== 14.6  fourteen units, from a fresh clone =="
sh scripts/u14_fresh_clone.sh
