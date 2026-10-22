// Challenge 11.6, the payoff: the session, replanned.
//
// Unit 10's shop -- 20,000 customers in 12 cities, 200,000 orders, a few
// customers ordering far more than the rest -- with three secondary indexes.
// A session of questions is asked twice in the same process, over the same
// pages: once planned as Unit 10 planned (literal planning), once by this
// unit's planner, the two alternating run for run (tests/shop.hpp). Each
// answer is printed with the new plan, and beside each operator's rows the
// rows the planner expected; both plans must return the same rows. Timings
// are medians of 5 runs.
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "buffer_pool.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "pager.hpp"
#include "shop.hpp"
#include "show.hpp"

using namespace labdb;
using labdb::test::now_s;

namespace {

void indent(const std::string& text, const char* pad) {
  std::string line;
  for (char c : text) {
    line += c;
    if (c == '\n') {
      std::printf("%s%s", pad, line.c_str());
      line.clear();
    }
  }
}

struct Line {
  std::string label;
  labdb::test::Race r;
};
std::vector<Line> g_summary;

void ask(Executor& e, const std::string& label, const std::string& sql) {
  std::printf("\n  %s\n  SQL> %s\n", label.c_str(), sql.c_str());
  const labdb::test::Race r = labdb::test::race(e, sql);
  std::fflush(stdout);
  char* buf = nullptr;
  std::size_t len = 0;
  std::FILE* mem = open_memstream(&buf, &len);
  labdb::test::print_table(r.result, 2, mem);
  std::fclose(mem);
  indent(std::string(buf, len), "    ");
  std::free(buf);
  std::printf("  Unit 10's plan: %.2f ms.  This unit's: %.3f ms -- its plan, rows produced and expected:\n",
              r.before_ms, r.after_ms);
  indent(e.explain_last(true), "    ");
  g_summary.push_back({label, r});
}

}  // namespace

int main() {
  const char* path = "u11_session_bench.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 8192);  // every page stays cached
  Catalog cat(pool);
  Executor e(cat);
  labdb::test::build_shop(e);
  for (const char* ddl : {"CREATE INDEX ON orders (customer)", "CREATE INDEX ON orders (cents)",
                          "CREATE INDEX ON customers (city)"})
    e.run_sql(ddl);
  std::printf("\nUnit 10's shop, with indexes on orders(customer), orders(cents) and customers(city).\n");
  std::printf("Each question planned as Unit 10 planned it, then by this unit's planner:\n");

  ask(e, "1. one customer's orders",
      "SELECT o.oid, o.cents FROM orders o JOIN customers c ON o.customer = c.id WHERE c.id = 7 "
      "ORDER BY o.cents DESC LIMIT 3");
  ask(e, "2. whoever placed one order",
      "SELECT c.name, c.city, o.cents FROM orders o JOIN customers c ON o.customer = c.id WHERE o.oid = 123456");
  ask(e, "3. a small city's revenue",
      "SELECT COUNT(*), SUM(o.cents) FROM orders o JOIN customers c ON o.customer = c.id "
      "WHERE c.city = 'Zagreb'");
  ask(e, "4. the largest city's revenue",
      "SELECT COUNT(*), SUM(o.cents) FROM orders o JOIN customers c ON o.customer = c.id "
      "WHERE c.city = 'Cairo'");
  ask(e, "5. the biggest orders, and who placed them",
      "SELECT c.name, o.cents FROM orders o JOIN customers c ON o.customer = c.id WHERE o.cents > 99000");
  ask(e, "6. every order of one order's customer",
      "SELECT COUNT(*) FROM orders a JOIN orders b ON a.customer = b.customer WHERE a.oid = 4242");
  ask(e, "7. the extremes", "SELECT MIN(oid), MAX(oid), MIN(cents), MAX(cents) FROM orders");
  ask(e, "8. revenue by city (nothing to choose)",
      "SELECT c.city, COUNT(*) AS orders, SUM(o.cents) AS revenue FROM orders o "
      "JOIN customers c ON o.customer = c.id GROUP BY c.city ORDER BY revenue DESC LIMIT 3");

  std::printf("\nthe session, side by side -- median (fastest-slowest) of 5 runs, in ms:\n\n");
  std::printf("  %-42s %-23s %-23s %s\n", "question", "Unit 10", "Unit 11", "speedup");
  for (const Line& l : g_summary) {
    char b[48], a[48];
    std::snprintf(b, sizeof b, "%.1f (%.1f-%.1f)", l.r.before_ms, l.r.before_lo, l.r.before_hi);
    std::snprintf(a, sizeof a, "%.3f (%.3f-%.3f)", l.r.after_ms, l.r.after_lo, l.r.after_hi);
    std::printf("  %-42s %-23s %-23s %6.0fx\n", l.label.c_str(), b, a, l.r.before_ms / l.r.after_ms);
  }
  ::unlink(path);
  return 0;
}
