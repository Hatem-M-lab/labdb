// Challenge 12.6, the payoff: the sessions of Units 10 and 11, run three ways.
//
// Unit 10's shop -- 20,000 customers in 12 cities, 200,000 orders -- with Unit
// 11's three secondary indexes, and the questions both units asked of it. Each
// question is planned once, by the planner, and the plan is run three ways in
// the same process over the same pages, alternating run for run: as Unit 11
// ran it (every row's page copied out of the pool, a row at a time); with rows
// read where they lie, still a row at a time (Challenge 12.2); and in batches
// of 1,024 rows (Challenge 12.3 on). All three must return the same rows in
// the same order. Between runs, an untimed one-row query, so that tearing down
// the last plan is not charged to the next. Medians of 5 runs, with the
// fastest and slowest. Then the plan is run once more as the engine runs it
// by default, rows or batches by the planner's estimates (Executor::kChoose),
// and the summary sets the way it chose against Unit 11's.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

std::string exact(const ResultSet& r) {
  std::string s;
  for (const auto& row : r.rows) {
    for (const Field& f : row) s += labdb::test::cell_text(f, 6) + "|";
    s += "\n";
  }
  return s;
}

struct Way {
  const char* name;
  bool copy;
  std::size_t batch;
};
const Way kWays[] = {{"Unit 11", true, 0}, {"rows in place", false, 0}, {"batches", false, kDefaultBatchRows}};

struct Times {
  double median[3], lo[3], hi[3];
};
struct Line {
  std::string label;
  Times t;
  bool batches;  // the engine's own choice
};
std::vector<Line> g_summary;

void ask(Catalog& cat, Executor& e, const std::string& label, const std::string& sql) {
  std::printf("\n  %s\n  SQL> %s\n", label.c_str(), sql.c_str());
  std::vector<double> t[3];
  std::string answer[3];
  ResultSet shown;
  for (int r = 0; r <= 5; ++r)
    for (int w = 0; w < 3; ++w) {
      cat.set_copy_pages(kWays[w].copy);
      e.set_batch_rows(kWays[w].batch);
      e.run_sql("SELECT id FROM customers WHERE id = 0");
      const double t0 = now_s();
      ResultSet res = e.run_sql(sql);
      const double dt = now_s() - t0;
      if (r == 0) {  // the first round only warms
        answer[w] = exact(res);
        if (w == 2) shown = std::move(res);
        continue;
      }
      t[w].push_back(dt * 1e3);
    }
  cat.set_copy_pages(false);
  e.set_batch_rows(Executor::kChoose);
  if (answer[0] != answer[1] || answer[1] != answer[2]) std::abort();  // the three ways disagree
  e.run_sql(sql);  // once more as the engine chooses, for explain_last()
  const bool batches = e.last_batch_rows() > 0;
  std::fflush(stdout);
  char* buf = nullptr;
  std::size_t len = 0;
  std::FILE* mem = open_memstream(&buf, &len);
  labdb::test::print_table(shown, 2, mem);
  std::fclose(mem);
  indent(std::string(buf, len), "    ");
  std::free(buf);
  Times out;
  for (int w = 0; w < 3; ++w) {
    std::sort(t[w].begin(), t[w].end());
    out.median[w] = t[w][t[w].size() / 2];
    out.lo[w] = t[w].front();
    out.hi[w] = t[w].back();
  }
  std::printf("  Unit 11's way %.3f ms, rows in place %.3f ms, batches %.3f ms -- the plan, which the engine "
              "runs %s:\n", out.median[0], out.median[1], out.median[2],
              batches ? "in batches" : "a row at a time");
  indent(e.explain_last(true), "    ");
  g_summary.push_back({label, out, batches});
}

}  // namespace

int main() {
  const char* path = "u12_session_bench.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 8192);  // every page stays cached
  Catalog cat(pool);
  Executor e(cat);
  labdb::test::build_shop(e);
  for (const char* ddl : {"CREATE INDEX ON orders (customer)", "CREATE INDEX ON orders (cents)",
                          "CREATE INDEX ON customers (city)"})
    e.run_sql(ddl);
  std::printf("\nUnit 10's shop, with Unit 11's indexes on orders(customer), orders(cents) and customers(city).\n");
  std::printf("Each question planned once, and run Unit 11's way, with rows read in place, and in batches:\n");

  // Unit 10's session
  ask(cat, e, "1. the orders, summarized",
      "SELECT COUNT(*), SUM(cents), AVG(cents), MIN(cents), MAX(cents) FROM orders");
  ask(cat, e, "2. the five biggest spenders",
      "SELECT c.name, COUNT(*) AS orders, SUM(o.cents) AS spent FROM orders o JOIN customers c ON o.customer = c.id "
      "GROUP BY c.id, c.name ORDER BY spent DESC LIMIT 5");
  ask(cat, e, "3. revenue by city",
      "SELECT c.city, COUNT(*) AS orders, SUM(o.cents) AS revenue, AVG(o.cents) AS avg_order FROM orders o "
      "JOIN customers c ON o.customer = c.id GROUP BY c.city ORDER BY revenue DESC");
  ask(cat, e, "4. the three busiest days",
      "SELECT day, COUNT(*) AS n FROM orders GROUP BY day ORDER BY n DESC, day LIMIT 3");
  ask(cat, e, "5. customers with 250 orders or more",
      "SELECT customer, COUNT(*) AS n FROM orders GROUP BY customer HAVING COUNT(*) >= 250 ORDER BY n DESC");
  // Unit 11's session
  ask(cat, e, "6. one customer's orders",
      "SELECT o.oid, o.cents FROM orders o JOIN customers c ON o.customer = c.id WHERE c.id = 7 "
      "ORDER BY o.cents DESC LIMIT 3");
  ask(cat, e, "7. whoever placed one order",
      "SELECT c.name, c.city, o.cents FROM orders o JOIN customers c ON o.customer = c.id WHERE o.oid = 123456");
  ask(cat, e, "8. a small city's revenue",
      "SELECT COUNT(*), SUM(o.cents) FROM orders o JOIN customers c ON o.customer = c.id WHERE c.city = 'Zagreb'");
  ask(cat, e, "9. the largest city's revenue",
      "SELECT COUNT(*), SUM(o.cents) FROM orders o JOIN customers c ON o.customer = c.id WHERE c.city = 'Cairo'");
  ask(cat, e, "10. the biggest orders, and their buyers",
      "SELECT c.name, o.cents FROM orders o JOIN customers c ON o.customer = c.id WHERE o.cents > 99000");
  ask(cat, e, "11. all orders of one order's customer",
      "SELECT COUNT(*) FROM orders a JOIN orders b ON a.customer = b.customer WHERE a.oid = 4242");
  ask(cat, e, "12. the extremes", "SELECT MIN(oid), MAX(oid), MIN(cents), MAX(cents) FROM orders");

  std::printf("\nthe sessions, side by side -- median (fastest-slowest) of 5 runs, in ms:\n\n");
  std::printf("  %-40s %-22s %-22s %-22s %-9s %s\n", "question", "Unit 11's way", "rows in place", "batches",
              "chosen", "vs Unit 11");
  int right = 0;
  for (const Line& l : g_summary) {
    char cell[3][64];
    for (int w = 0; w < 3; ++w)
      std::snprintf(cell[w], sizeof cell[w], l.t.median[w] < 1 ? "%.3f (%.3f-%.3f)" : "%.1f (%.1f-%.1f)",
                    l.t.median[w], l.t.lo[w], l.t.hi[w]);
    const int way = l.batches ? 2 : 1;
    right += l.t.median[way] <= l.t.median[3 - way];
    std::printf("  %-40s %-22s %-22s %-22s %-9s %5.1fx\n", l.label.c_str(), cell[0], cell[1], cell[2],
                l.batches ? "batches" : "rows", l.t.median[0] / l.t.median[way]);
  }
  std::printf("  (vs Unit 11: Unit 11's time over the time of the way chosen; the faster way chosen for %d of "
              "%zu)\n", right, g_summary.size());
  ::unlink(path);
  return 0;
}
