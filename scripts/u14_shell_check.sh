#!/bin/sh
# Unit 14: the shell, from the outside. A session piped into bin/labdb must
# print exactly what is written below -- its tables, its errors and their
# carets, a transaction rolled back, a plan; and a shell killed with kill -9
# in the middle of a transaction must leave a database that opens with every
# transaction it committed, and without the one it had open.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
make -C "$root" bin/labdb >/dev/null
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"

cat > session.sql <<'SQL'
CREATE TABLE accounts (id INT64 NOT NULL, owner TEXT, cents INT64);
INSERT INTO accounts VALUES (1, 'ada', 10000);
INSERT INTO accounts VALUES (2, 'brian', -2500);
INSERT INTO accounts VALUES (3, 'grace', NULL);
SELECT * FROM accounts WHERE cents < 0;
BEGIN;
INSERT INTO accounts VALUES (4, 'edsger', 700);
SELECT COUNT(*), SUM(cents) FROM accounts;
ROLLBACK;
SELECT COUNT(*), SUM(cents), COUNT(cents) FROM accounts;
SELECT owner FROM acounts;
SELECT owner FROM accounts WHERE cents > 'a lot';
SELECT owner, cents FROM accounts ORDER BY cents DESC LIMIT -1;
SELECT owner, cents FROM accounts ORDER BY cents DESC LIMIT 2;
.tables
.explain
SQL
cat > expected.txt <<'OUT'
id  owner  cents
--  -----  -----
 2  brian  -2500
(1 row)

COUNT(*)  SUM(cents)
--------  ----------
       4        8200
(1 row)

COUNT(*)  SUM(cents)  COUNT(cents)
--------  ----------  ------------
       3        7500             2
(1 row)

error: no such table: acounts
error: type mismatch: cents is INT64, compared with 'a lot'
error: parse error at line 1, column 61: LIMIT must not be negative
    SELECT owner, cents FROM accounts ORDER BY cents DESC LIMIT -1
                                                                ^
owner  cents
-----  -----
grace   NULL
ada    10000
(2 rows)

accounts: 3 rows
Project owner, cents                                        rows=2
  Sort cents DESC (top 2)                                   rows=2
      rows held: 2 of 3 read, 1 replacements
    SeqScan accounts                                        rows=3 est=3
OUT
"$root/bin/labdb" bank.db < session.sql > got.txt 2>&1 || true
if ! cmp -s expected.txt got.txt; then
  echo "u14_shell_check: the session printed something else:"
  diff expected.txt got.txt || true
  exit 1
fi
echo "PASS session: $(grep -c . session.sql) lines in, the tables, errors, carets and plan expected out"

# A shell killed in the middle of a transaction.
mkfifo in
"$root/bin/labdb" pay.db < in > shell.txt 2>&1 &
pid=$!
exec 3> in
echo "CREATE TABLE payments (id INT64 NOT NULL, cents INT64);" >&3
for i in 1 2 3; do echo "INSERT INTO payments VALUES ($i, $((i * 100)));" >&3; done
echo "BEGIN;" >&3
echo "INSERT INTO payments VALUES (4, 400);" >&3
echo "SELECT COUNT(*), SUM(cents) FROM payments;" >&3
n=0
while ! grep -q "(1 row)" shell.txt 2>/dev/null; do  # the shell has answered: everything before is done
  n=$((n + 1))
  if [ "$n" -gt 400 ]; then echo "u14_shell_check: the shell did not answer"; exit 1; fi
  sleep 0.05
done
kill -9 "$pid"
wait "$pid" 2>/dev/null || true
exec 3>&-
"$root/bin/labdb" --plain pay.db -c "SELECT COUNT(*), SUM(cents) FROM payments" > after.txt 2>&1
printf 'recovered: 5 transactions from the log\n3|600\n' > want.txt
if ! cmp -s want.txt after.txt; then
  echo "u14_shell_check: after kill -9, the database opened as:"
  cat after.txt
  exit 1
fi
echo "PASS kill -9: inside its transaction the shell saw $(sed -n 3p shell.txt | awk '{ print $1 }') rows;" \
     "reopened, the 5 transactions it committed, 3 rows, and not the one it had open"
