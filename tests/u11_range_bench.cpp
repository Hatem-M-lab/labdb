// Challenge 11.1: what a range of the key costs.
//
// Unit 10's orders, 200,000 rows. SELECT SUM(cents) FROM orders WHERE
// oid >= 10000 AND oid < 10000 + W, for W from 1 to 190,000: planned as Unit
// 10 planned it -- every row read, and the WHERE checked on each -- and by
// this unit's planner, which reads the key's own B+Tree from the first key in
// the range to the last. Then ORDER BY the key with a LIMIT, and MIN and MAX
// of the key. Medians of 5 runs, the two planners alternating.
#include <unistd.h>

#include <cstdio>
#include <string>

#include "buffer_pool.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "pager.hpp"
#include "shop.hpp"

using namespace labdb;
using labdb::test::race;

namespace {

// The first line of the plan's access to orders: what this unit's planner chose.
std::string access_of(const std::string& plan) {
  for (const char* op : {"IndexLookup", "IndexScan", "IndexMinMax", "SeqScan"}) {
    const std::size_t at = plan.find(op);
    if (at == std::string::npos) continue;
    std::string line = plan.substr(at, plan.find('\n', at) - at);
    return line.substr(0, line.find("  "));
  }
  return "?";
}

}  // namespace

int main() {
  const char* path = "u11_range_bench.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 8192);  // every page stays cached
  Catalog cat(pool);
  Executor e(cat);
  labdb::test::build_shop(e);

  std::printf("\nSELECT SUM(cents) FROM orders WHERE oid >= 10000 AND oid < 10000 + W (median of 5 runs):\n\n");
  std::printf("  %8s %8s %12s %12s  %s\n", "W", "rows", "Unit 10", "Unit 11", "Unit 11's access to orders");
  for (long w : {1L, 10L, 100L, 1000L, 10000L, 100000L, 190000L}) {
    const std::string sql = "SELECT SUM(cents) FROM orders WHERE oid >= 10000 AND oid < " + std::to_string(10000 + w);
    const labdb::test::Race r = race(e, sql);
    std::printf("  %8ld %8ld %9.2f ms %9.3f ms  %s\n", w, w, r.before_ms, r.after_ms,
                access_of(e.explain_last()).c_str());
  }

  std::printf("\nthe key's order, and its ends:\n");
  for (const char* sql : {"SELECT oid, cents FROM orders ORDER BY oid LIMIT 10",
                          "SELECT oid, cents FROM orders ORDER BY oid DESC LIMIT 10",
                          "SELECT MIN(oid), MAX(oid) FROM orders"}) {
    const labdb::test::Race r = race(e, sql);
    std::printf("\n  SQL> %s\n  Unit 10: %.2f ms.  Unit 11: %.3f ms:\n", sql, r.before_ms, r.after_ms);
    const std::string plan = e.explain_last();
    std::size_t at = 0;
    while (at < plan.size()) {
      const std::size_t end = plan.find('\n', at);
      std::printf("    %s\n", plan.substr(at, end - at).c_str());
      at = end + 1;
    }
  }
  ::unlink(path);
  return 0;
}
