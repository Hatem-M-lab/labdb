// A probe, not a benchmark: one stage of a point lookup, repeated, for
// counting instructions under valgrind --tool=callgrind.
//
//   u10_query_probe STAGE N      STAGE: parse | plan | query | insert
//
// It parses, or plans, or runs end to end, `SELECT customer FROM orders
// WHERE oid = 42` N times against a 100-row table -- or (insert, added in
// Unit 11) runs N INSERTs of new rows into it. Run with N = 0 and with a
// large N, and the difference over N is that stage's instructions per query,
// with the setup cancelled out. Timing a difference of a few percent on a
// shared machine is hopeless (Challenge 10.1); counting instructions is not.
// It uses only Parser, Executor::plan and run_sql, so the same file compiles
// against Unit 9's engine and this one (scripts/u10_before_after.sh does both).
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "buffer_pool.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "pager.hpp"
#include "parser.hpp"

using namespace labdb;

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s parse|plan|query|insert N\n", argv[0]);
    return 2;
  }
  const std::string stage = argv[1];
  const long n = std::atol(argv[2]);
  const char* path = "u10_query_probe.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 64);
  Catalog cat(pool);
  Executor e(cat);
  e.run_sql("CREATE TABLE orders (oid INT64 NOT NULL, customer INT64, cents INT64)");
  for (int i = 0; i < 100; ++i)
    e.run_sql("INSERT INTO orders VALUES (" + std::to_string(i) + ", " + std::to_string(i % 7) + ", 100)");
  const std::string sql = "SELECT customer FROM orders WHERE oid = 42";
  Parser p0(sql);
  const Statement parsed = p0.parse_single();
  for (long i = 0; i < n; ++i) {
    if (stage == "insert") {
      ResultSet r = e.run_sql("INSERT INTO orders VALUES (" + std::to_string(1000 + i) + ", " +
                              std::to_string(i % 7) + ", 100)");
      labdb::test::do_not_optimize(r);
    } else if (stage == "parse") {
      Parser p(sql);
      Statement s = p.parse_single();
      labdb::test::do_not_optimize(s);
    } else if (stage == "plan") {
      Executor::Plan plan = e.plan(parsed.select);
      labdb::test::do_not_optimize(plan);
    } else {
      ResultSet r = e.run_sql(sql);
      labdb::test::do_not_optimize(r);
    }
  }
  ::unlink(path);
  return 0;
}
