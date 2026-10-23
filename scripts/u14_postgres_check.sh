#!/bin/sh
# Unit 14: labdb against PostgreSQL. Starts a private PostgreSQL server in a
# temporary directory -- initdb with the C locale, so that text compares byte
# by byte as labdb's does -- runs bin/u14_pg_diff against it, and stops it.
# Needs PostgreSQL's server programs (initdb, pg_ctl) and psql. initdb will
# not run as root; as root, the server runs as the postgres user.
#
#   sh scripts/u14_postgres_check.sh [QUERIES]
#
# PG_DIFF names another build of the comparison to run instead of
# bin/u14_pg_diff (scripts/u14_before_after.sh and u14_mutation_check.sh
# build it against other engines).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
if [ -z "$PG_DIFF" ]; then
  make bin/u14_pg_diff >/dev/null
  PG_DIFF=$root/bin/u14_pg_diff
fi
bin=$(dirname "$(command -v initdb 2>/dev/null || ls /usr/lib/postgresql/*/bin/initdb 2>/dev/null | tail -1)")
if [ ! -x "$bin/initdb" ] || ! command -v psql >/dev/null 2>&1; then
  echo "PostgreSQL (initdb, pg_ctl, psql) not found: skipped"
  exit 0
fi
tmp=$(mktemp -d /tmp/labdb-pg.XXXXXX)
chmod 755 "$tmp"
if [ "$(id -u)" = 0 ]; then
  chown postgres "$tmp"
  as() { su postgres -c "$1"; }
else
  as() { sh -c "$1"; }
fi
port=$((50000 + $$ % 10000))
as "'$bin/initdb' -D '$tmp/data' --locale=C --encoding=UTF8 -A trust -U labdb >/dev/null"
as "'$bin/pg_ctl' -D '$tmp/data' -o \"-k '$tmp' -p $port -c listen_addresses= -c fsync=off\" -l '$tmp/log' -w start >/dev/null"
trap 'as "\"$bin/pg_ctl\" -D \"$tmp/data\" -m fast -w stop >/dev/null"; rm -rf "$tmp"' EXIT
mkdir "$tmp/run"
cd "$tmp/run"
PGHOST=$tmp PGPORT=$port PGUSER=labdb PGDATABASE=postgres "$PG_DIFF" "$@"
