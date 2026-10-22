// A probe, not a benchmark: INSERTs into a table with 0 to 3 secondary
// indexes, for counting instructions under valgrind --tool=callgrind.
//
//   u11_insert_probe N INDEXES
//
// It builds orders (oid, customer, cents, day) with 20,000 rows and INDEXES
// secondary indexes (on customer, cents and day, in that order), then runs N
// more INSERTs. Run with N = 0 and with a large N, and the difference over N
// is one INSERT's instructions, with the setup cancelled out. A time per
// INSERT moves by a third from run to run on this machine (Challenge 11.2);
// an instruction count does not move at all.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#include "buffer_pool.hpp"
#include "executor.hpp"
#include "pager.hpp"

using namespace labdb;

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s N INDEXES\n", argv[0]);
    return 2;
  }
  const long n = std::atol(argv[1]);
  const int indexes = std::atoi(argv[2]);
  const char* path = "u11_insert_probe.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 8192);
  Catalog cat(pool);
  Executor e(cat);
  e.run_sql("CREATE TABLE orders (oid INT64 NOT NULL, customer INT64, cents INT64, day INT32)");
  const char* ddl[] = {"CREATE INDEX ON orders (customer)", "CREATE INDEX ON orders (cents)",
                       "CREATE INDEX ON orders (day)"};
  for (int k = 0; k < indexes; ++k) e.run_sql(ddl[k]);
  std::mt19937_64 rng(1107);
  const auto insert = [&](long i) {
    const std::uint64_t u = rng() % 20000;
    e.run_sql("INSERT INTO orders VALUES (" + std::to_string(i) + ", " + std::to_string(u * u / 20000) + ", " +
              std::to_string(100 + (rng() % 1000) * (rng() % 1000) / 10) + ", " + std::to_string(1 + rng() % 365) +
              ")");
  };
  for (long i = 0; i < 20000; ++i) insert(i);
  for (long i = 0; i < n; ++i) insert(20000 + i);
  ::unlink(path);
  return 0;
}
