// A probe, not a test: how does the executor answer comparisons with NULL
// when some ages are 0? Built against Unit 9's executor it prints 0 rows for
// all three queries, as SQL requires -- NULL is never equal, unequal, or
// greater than anything. Built against Unit 8's (see Challenge 9.1) it prints
// 2, 1 and 1: Unit 8 typed the NULL literal as the integer 0 and compared it.
// It uses only run_sql, so the same file compiles against either executor.
#include <unistd.h>

#include <cstdio>

#include "buffer_pool.hpp"
#include "executor.hpp"
#include "pager.hpp"

using namespace labdb;

int main() {
  const char* path = "u09_null_probe.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 64);
  Catalog cat(pool);
  Executor e(cat);
  e.run_sql("CREATE TABLE p (id INT64 NOT NULL, age INT32)");
  e.run_sql("INSERT INTO p VALUES (1, 0)");
  e.run_sql("INSERT INTO p VALUES (2, 0)");
  e.run_sql("INSERT INTO p VALUES (3, 41)");
  e.run_sql("INSERT INTO p VALUES (4, NULL)");
  const char* queries[] = {"SELECT id FROM p WHERE age = NULL",
                           "SELECT id FROM p WHERE age <> NULL",
                           "SELECT id FROM p WHERE age > NULL"};
  for (const char* q : queries)
    std::printf("%-38s -> %zu rows\n", q, e.run_sql(q).row_count());
  ::unlink(path);
  return 0;
}
