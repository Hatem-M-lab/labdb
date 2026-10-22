// Challenge 11.4: planning a join.
//
// Unit 10's shop. First the build side: one join, every customer to every
// order, planned as Unit 10 planned it -- the hash table built from the table
// written second, whatever its size -- and by this unit's planner, which
// builds from the smaller input. Medians of 5 runs, the two alternating.
//
// Then the choice between algorithms. The orders of K customers: an index
// nested-loop join searches the index on orders(customer) once per customer;
// a hash join reads every order once and probes a table of K customers. Timed
// both ways, forced, for K from 1 to all 20,000 -- once for the customers who
// order most (the lowest ids) and once for those who order least (the
// highest) -- beside the plan the planner chose and the number of joined rows
// it expected. Medians of 7 runs, with the fastest and the slowest.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "buffer_pool.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "pager.hpp"
#include "shop.hpp"

using namespace labdb;
using labdb::test::now_s;
using Algo = Executor::JoinAlgorithm;

namespace {

void print_plan(const std::string& plan) {
  for (std::size_t at = 0; at < plan.size();) {
    const std::size_t end = plan.find('\n', at);
    std::printf("    %s\n", plan.substr(at, end - at).c_str());
    at = end + 1;
  }
}

struct Timing {
  double median, lo, hi;  // ms
};

Timing time_ms(Executor& e, const std::string& sql, Algo algo) {
  e.set_join_algorithm(algo);
  std::vector<double> t;
  for (int r = 0; r <= 7; ++r) {
    e.run_sql("SELECT id FROM customers WHERE id = 0");  // untimed: the last plan's teardown
    const double t0 = now_s();
    e.run_sql(sql);
    if (r > 0) t.push_back(now_s() - t0);  // run 0 warms
  }
  e.set_join_algorithm(Algo::kAuto);
  std::sort(t.begin(), t.end());
  return {t[t.size() / 2] * 1e3, t.front() * 1e3, t.back() * 1e3};
}

// "rows=A est=B" on the plan's join line
void join_counts(const std::string& plan, double& rows, double& est) {
  const std::size_t line = plan.find("Join ON");
  const std::size_t r = plan.find("rows=", line), e = plan.find("est=", line);
  rows = std::stod(plan.substr(r + 5));
  est = std::stod(plan.substr(e + 4));
}

}  // namespace

int main() {
  const char* path = "u11_join_bench.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 8192);  // every page stays cached
  Catalog cat(pool);
  Executor e(cat);
  labdb::test::build_shop(e);
  e.run_sql("CREATE INDEX ON orders (customer)");

  // ---- the build side ----
  const std::string all = "SELECT COUNT(*) FROM customers c JOIN orders o ON o.customer = c.id";
  std::printf("\nthe build side:\n  SQL> %s\n", all.c_str());
  e.set_literal_planning(true);
  e.run_sql(all);
  std::printf("  Unit 10's plan:\n");
  print_plan(e.explain_last());
  e.set_literal_planning(false);
  const labdb::test::Race r = labdb::test::race(e, all);
  std::printf("  Unit 11's plan:\n");
  print_plan(e.explain_last(true));
  std::printf("  Unit 10: %.2f ms.  Unit 11: %.2f ms (median of 5 runs, alternating)\n", r.before_ms, r.after_ms);

  // ---- index nested loop or hash join ----
  std::printf("\nthe orders of K customers: SELECT COUNT(*), SUM(o.cents) FROM customers c JOIN orders o\n"
              "ON o.customer = c.id WHERE c.id < K (the customers who order most) or c.id >= 20000 - K\n"
              "(those who order least); each join forced, in ms, median (fastest-slowest) of 7 runs,\n"
              "then the planner's choice:\n\n");
  std::printf("  %6s  %-5s %7s %7s  %-20s %-20s %-6s %s\n", "K", "end", "rows", "est.", "index loop", "hash join",
              "faster", "planner");
  for (int k : {1, 10, 100, 1000, 2000, 5000, 10000, 20000}) {
    for (bool heavy : {true, false}) {
      if (k == 20000 && !heavy) continue;  // all 20,000 is one set of customers
      const std::string sql = "SELECT COUNT(*), SUM(o.cents) FROM customers c JOIN orders o ON o.customer = c.id "
                              "WHERE c.id " + std::string(heavy ? "< " : ">= ") +
                              std::to_string(heavy ? k : 20000 - k);
      const Timing loop = time_ms(e, sql, Algo::kIndexNestedLoop);
      const Timing hash = time_ms(e, sql, Algo::kHashLeft);
      e.run_sql(sql);
      const std::string plan = e.explain_last(true);
      double rows = 0, est = 0;
      join_counts(plan, rows, est);
      const bool chose_loop = plan.find("IndexNestedLoopJoin") != std::string::npos;
      char a[32], b[32];
      std::snprintf(a, sizeof a, "%.2f (%.2f-%.2f)", loop.median, loop.lo, loop.hi);
      std::snprintf(b, sizeof b, "%.2f (%.2f-%.2f)", hash.median, hash.lo, hash.hi);
      std::printf("  %6d  %-5s %7.0f %7.0f  %-20s %-20s %-6s %s\n", k, heavy ? "most" : "least", rows, est, a, b,
                  loop.median < hash.median ? "loop" : "hash", chose_loop ? "loop" : "hash");
    }
  }
  ::unlink(path);
  return 0;
}
