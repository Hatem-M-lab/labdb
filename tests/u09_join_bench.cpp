// The payoff: joins, spoken in SQL, and the experiment this unit exists for.
//
// First a session of join queries against Unit 8's customers and orders,
// each printed with the plan the engine ran. Then the golden experiment: two
// tables of n rows, joined on a key that matches each row of one to exactly
// one row of the other, as a nested-loop join and as a hash join, for n from
// 1,000 to 100,000. Both plans read the same tables and return the same n
// rows; the only difference is the algorithm in the middle.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "buffer_pool.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "pager.hpp"

using namespace labdb;
using labdb::test::now_s;
using Algo = Executor::JoinAlgorithm;

namespace {

void print_table(const ResultSet& r, std::size_t max_rows = 5) {
  std::string header;
  for (std::size_t i = 0; i < r.columns.size(); ++i) header += (i ? " | " : "") + r.columns[i];
  std::printf("    %s\n", header.c_str());
  for (std::size_t k = 0; k < r.rows.size() && k < max_rows; ++k) {
    std::string line;
    for (std::size_t i = 0; i < r.rows[k].size(); ++i) {
      const Field& f = r.rows[k][i];
      const std::string cell = f.is_null ? "NULL"
                               : f.type == ColType::kText ? f.text
                               : f.type == ColType::kBool ? (f.i64 ? "TRUE" : "FALSE")
                                                          : std::to_string(f.i64);
      line += (i ? " | " : "") + cell;
    }
    std::printf("    %s\n", line.c_str());
  }
  if (r.rows.size() > max_rows) std::printf("    ... (%zu rows total)\n", r.rows.size());
}

void show(Executor& e, const std::string& sql) {
  std::printf("  SQL> %s\n", sql.c_str());
  print_table(e.run_sql(sql));
  std::printf("  plan, after running:\n");
  const std::string plan = e.explain_last();
  std::size_t start = 0;
  while (start < plan.size()) {
    const std::size_t end = plan.find('\n', start);
    std::printf("    %s\n", plan.substr(start, end - start).c_str());
    start = end + 1;
  }
  std::printf("\n");
}

// Seconds for one run of `sql` under `algo`; the row count must be `expect`.
double time_join(Executor& e, Algo algo, const std::string& sql, std::size_t expect) {
  e.set_join_algorithm(algo);
  const double t0 = now_s();
  const std::size_t n = e.run_sql(sql).row_count();
  const double s = now_s() - t0;
  REQUIRE(n == expect);
  e.set_join_algorithm(Algo::kAuto);
  return s;
}

double median_of(Executor& e, Algo algo, const std::string& sql, std::size_t expect, int reps) {
  std::vector<double> t;
  for (int r = 0; r < reps; ++r) t.push_back(time_join(e, algo, sql, expect));
  std::sort(t.begin(), t.end());
  return t[t.size() / 2];
}

std::string seconds(double s) {
  char buf[32];
  if (s < 1.0) std::snprintf(buf, sizeof buf, "%.1f ms", s * 1e3);
  else std::snprintf(buf, sizeof buf, "%.1f s", s);
  return buf;
}

}  // namespace

int main() {
  const char* path = "u09_join_bench.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 16384);  // big enough that every page stays cached
  Catalog cat(pool);
  Executor e(cat);

  // ---- a session of joins over Unit 8's database ----
  e.run_sql("CREATE TABLE customers (id INT64 NOT NULL, name TEXT, vip BOOL)");
  e.run_sql("CREATE TABLE orders (oid INT64 NOT NULL, customer INT32 NOT NULL, cents INT32 NOT NULL)");
  const int kCustomers = 20000, kOrders = 200000;
  e.run_sql("BEGIN");  // Unit 13: one transaction, so one commit, for the whole load
  for (int i = 0; i < kCustomers; ++i)
    e.run_sql("INSERT INTO customers VALUES (" + std::to_string(i) + ", 'name_" +
              std::to_string(i) + "', " + (i % 10 == 0 ? "TRUE" : "FALSE") + ")");
  for (int i = 0; i < kOrders; ++i)
    e.run_sql("INSERT INTO orders VALUES (" + std::to_string(i) + ", " +
              std::to_string(i % kCustomers) + ", " + std::to_string((i * 7) % 100000) + ")");
  e.run_sql("COMMIT");

  std::printf("a SQL session with joins, against %d customers and %d orders:\n\n",
              kCustomers, kOrders);
  show(e, "SELECT o.oid, c.name, o.cents FROM orders o JOIN customers c "
          "ON o.customer = c.id WHERE o.cents < 50");
  show(e, "SELECT c.name, o.oid FROM customers c JOIN orders o "
          "ON o.customer = c.id WHERE c.id = 7");
  show(e, "SELECT a.oid, b.oid FROM orders a JOIN orders b "
          "ON a.customer = b.customer AND a.oid < b.oid WHERE a.oid < 3");

  // ---- the golden experiment ----
  const int sizes[] = {1000, 2000, 5000, 10000, 20000, 50000, 100000};
  std::mt19937_64 rng(99);
  for (int n : sizes) {
    const std::string A = "a" + std::to_string(n), B = "b" + std::to_string(n);
    e.run_sql("CREATE TABLE " + A + " (id INT64 NOT NULL, b_id INT64 NOT NULL, x INT32)");
    e.run_sql("CREATE TABLE " + B + " (id INT64 NOT NULL, y INT32)");
    std::vector<int> perm(n);
    std::iota(perm.begin(), perm.end(), 0);
    std::shuffle(perm.begin(), perm.end(), rng);  // a row i matches b row perm[i]
    e.run_sql("BEGIN");  // Unit 13: one transaction, so one commit, for the whole load
    for (int i = 0; i < n; ++i) {
      e.run_sql("INSERT INTO " + A + " VALUES (" + std::to_string(i) + ", " +
                std::to_string(perm[i]) + ", " + std::to_string(rng() % 1000) + ")");
      e.run_sql("INSERT INTO " + B + " VALUES (" + std::to_string(i) + ", " +
                std::to_string(rng() % 1000) + ")");
    }
    e.run_sql("COMMIT");
  }

  std::printf("the golden experiment: SELECT a.id, b.y FROM a JOIN b ON a.b_id = b.id\n"
              "two tables of n rows each; every a row matches exactly one b row\n\n");
  std::printf("  %9s  %17s  %11s  %10s  %9s  %12s  %12s\n", "n", "pairs compared",
              "nested loop", "hash join", "speedup", "NLJ ns/pair", "hash ns/row");
  for (int n : sizes) {
    const std::string sql = "SELECT a.id, b.y FROM a" + std::to_string(n) + " a JOIN b" +
                            std::to_string(n) + " b ON a.b_id = b.id";
    const std::size_t expect = static_cast<std::size_t>(n);
    const int nlj_reps = n <= 20000 ? 3 : 1;  // the largest runs take tens of seconds
    const double nlj = median_of(e, Algo::kNestedLoop, sql, expect, nlj_reps);
    const double hash = median_of(e, Algo::kHash, sql, expect, 5);
    const double pairs = static_cast<double>(n) * n;
    std::printf("  %9d  %17.0f  %11s  %10s  %8.0fx  %12.2f  %12.1f\n", n, pairs,
                seconds(nlj).c_str(), seconds(hash).c_str(), nlj / hash, nlj / pairs * 1e9,
                hash / (2.0 * n) * 1e9);
    std::fflush(stdout);
  }

  ::unlink(path);
  return 0;
}
