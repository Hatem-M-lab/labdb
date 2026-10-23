// Challenge 11.2: what a secondary index buys, and what it costs.
//
// Unit 10's shop. First the indexes are built over rows that already exist --
// how long CREATE INDEX takes, and how many pages it adds. Then one question,
// how many orders has customer c placed, for customers with very different
// numbers of orders, planned as Unit 10 planned it (a scan) and through the
// index; and two questions about cities, through the TEXT index on city, whose
// keys are 8-byte prefixes and whose rows are checked again. Medians of 5 runs,
// the two planners alternating.
//
// Then the price, paid on every write: 100,000 INSERTs into an orders table
// with no secondary index, then with one, two and three. Medians of 7 runs;
// scripts/u11_before_after.sh counts the same INSERTs' instructions, which do
// not move from run to run as these times do.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "buffer_pool.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "pager.hpp"
#include "shop.hpp"

using namespace labdb;
using labdb::test::now_s;
using labdb::test::race;

int main() {
  const char* path = "u11_index_bench.db";
  ::unlink(path);
  {
    Pager pager(path);
    BufferPool pool(pager, 8192);  // every page stays cached
    Catalog cat(pool);
    Executor e(cat);
    labdb::test::build_shop(e);
    std::printf("\nUnit 10's shop: 20000 customers, 200000 orders, %u pages\n\n", pool.page_count());
    for (const char* ddl : {"CREATE INDEX ON orders (customer)", "CREATE INDEX ON customers (city)"}) {
      const std::uint32_t pages = pool.page_count();
      const double t0 = now_s();
      e.run_sql(ddl);
      std::printf("  %-34s %6.1f ms, %4u pages\n", ddl, (now_s() - t0) * 1e3, pool.page_count() - pages);
    }

    std::map<std::int64_t, std::int64_t> orders_of;
    for (const auto& row : e.run_sql("SELECT customer, COUNT(*) FROM orders GROUP BY customer").rows)
      orders_of[row[0].i64] = row[1].i64;
    std::vector<std::int64_t> pick = {0, 7};
    const auto first_with = [&](std::int64_t n) {
      for (std::int64_t c = 0; c < 20000; ++c)
        if ((orders_of.count(c) ? orders_of[c] : 0) == n) return c;
      return std::int64_t{-1};
    };
    for (std::int64_t n : {10, 1, 0}) pick.push_back(first_with(n));

    std::printf("\nSELECT COUNT(*), SUM(cents) FROM orders WHERE customer = c (median of 5 runs):\n\n");
    std::printf("  %8s %8s %12s %12s\n", "c", "orders", "Unit 10", "Unit 11");
    for (std::int64_t c : pick) {
      const labdb::test::Race r =
          race(e, "SELECT COUNT(*), SUM(cents) FROM orders WHERE customer = " + std::to_string(c));
      std::printf("  %8lld %8lld %9.2f ms %9.3f ms\n", static_cast<long long>(c),
                  static_cast<long long>(r.result.rows[0][0].i64), r.before_ms, r.after_ms);
    }
    std::printf("  Unit 11's plan for the last of them:\n");
    const std::string plan = e.explain_last();
    for (std::size_t at = 0; at < plan.size();) {
      const std::size_t end = plan.find('\n', at);
      std::printf("    %s\n", plan.substr(at, end - at).c_str());
      at = end + 1;
    }

    for (const char* city : {"Zagreb", "Cairo"}) {
      const std::string sql = std::string("SELECT COUNT(*) FROM customers WHERE city = '") + city + "'";
      const labdb::test::Race r = race(e, sql);
      std::printf("\n  SQL> %s\n  %lld customers. Unit 10: %.2f ms.  Unit 11: %.3f ms:\n", sql.c_str(),
                  static_cast<long long>(r.result.rows[0][0].i64), r.before_ms, r.after_ms);
      const std::string p = e.explain_last();
      for (std::size_t at = 0; at < p.size();) {
        const std::size_t end = p.find('\n', at);
        std::printf("    %s\n", p.substr(at, end - at).c_str());
        at = end + 1;
      }
    }
  }
  ::unlink(path);

  // ------------------------------------------------- what an index costs
  constexpr int kInsertReps = 7;
  std::printf("\nthe price: 100000 INSERTs into orders, with 0 to 3 secondary indexes (median of %d runs):\n\n",
              kInsertReps);
  const char* ddl[] = {"CREATE INDEX ON orders (customer)", "CREATE INDEX ON orders (cents)",
                       "CREATE INDEX ON orders (day)"};
  double base = 0;
  for (int indexes = 0; indexes <= 3; ++indexes) {
    std::vector<double> runs;
    std::uint32_t grew = 0;
    for (int rep = 0; rep < kInsertReps; ++rep) {
      ::unlink(path);
      Pager pager(path);
      BufferPool pool(pager, 8192);
      Catalog cat(pool);
      Executor e(cat);
      e.run_sql("CREATE TABLE orders (oid INT64 NOT NULL, customer INT64, cents INT64, day INT32)");
      for (int k = 0; k < indexes; ++k) e.run_sql(ddl[k]);
      std::mt19937_64 rng(1006);
      std::vector<std::string> sql;
      for (int i = 0; i < 100000; ++i) {
        const std::uint64_t u = rng() % 20000;
        sql.push_back("INSERT INTO orders VALUES (" + std::to_string(i) + ", " + std::to_string(u * u / 20000) +
                      ", " + std::to_string(100 + (rng() % 1000) * (rng() % 1000) / 10) + ", " +
                      std::to_string(1 + rng() % 365) + ")");
      }
      const std::uint32_t pages = pool.page_count();
      const double t0 = now_s();
      e.run_sql("BEGIN");  // Unit 13: the INSERTs' own work, not a commit's fsync for each
      for (const std::string& s : sql) e.run_sql(s);
      e.run_sql("COMMIT");
      runs.push_back(now_s() - t0);
      grew = pool.page_count() - pages;  // the same in every run: the same rows, the same splits
    }
    std::sort(runs.begin(), runs.end());
    const double us = runs[kInsertReps / 2] * 1e6 / 100000;
    if (indexes == 0) base = us;
    std::printf("  %d index%s  %5.2f us per INSERT (%4.2f-%4.2f)  %+4.0f%%   the file grew %5u pages\n", indexes,
                indexes == 1 ? "  " : "es", us, runs.front() * 1e6 / 100000, runs.back() * 1e6 / 100000,
                (us / base - 1) * 100, grew);
  }
  ::unlink(path);
  return 0;
}
