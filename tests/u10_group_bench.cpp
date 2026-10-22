// Challenge 10.5, the payoff: questions that summarize, and what a group costs.
//
// First a short analytics session in SQL, against 20,000 customers and
// 200,000 orders: each result, the plan that produced it, and its time. Then
// the experiment. The same 2,000,000 rows (k, v), held in memory so that
// storage costs nothing, are grouped by k with COUNT(*) and SUM(v), while
// the number of distinct k grows from 10 to 1,000,000 -- the same input size,
// the same plan, the same work per row in the source code; only the number of
// groups the hash table has to hold changes. The session's timings are
// medians of 5 runs; the experiment's are medians of 7, printed with the
// fastest and slowest of the 7, because this machine's noise is part of what
// its numbers mean.
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
#include "operator.hpp"
#include "pager.hpp"
#include "show.hpp"

using namespace labdb;
using labdb::test::do_not_optimize;
using labdb::test::now_s;

namespace {

constexpr int kReps = 5;

// The sorted times of `reps` runs of `run`.
template <class F>
std::vector<double> times_s(F&& run, int reps) {
  std::vector<double> t;
  for (int r = 0; r < reps; ++r) {
    const double t0 = now_s();
    run();
    t.push_back(now_s() - t0);
  }
  std::sort(t.begin(), t.end());
  return t;
}

template <class F>
double median_s(F&& run) {
  return times_s(run, kReps)[kReps / 2];
}

std::string size_text(std::size_t bytes) {
  char buf[32];
  if (bytes < 1000000) std::snprintf(buf, sizeof buf, "%.1f KB", static_cast<double>(bytes) / 1e3);
  else std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / 1e6);
  return buf;
}

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

void session(Executor& e, const std::string& sql) {
  std::printf("\n  SQL> %s\n", sql.c_str());
  ResultSet r;
  const double t = median_s([&] { r = e.run_sql(sql); });
  std::FILE* out = stdout;
  // the table, indented like the rest of the session
  std::fflush(out);
  char* buf = nullptr;
  std::size_t len = 0;
  std::FILE* mem = open_memstream(&buf, &len);
  labdb::test::print_table(r, 2, mem);
  std::fclose(mem);
  indent(std::string(buf, len), "    ");
  std::free(buf);
  std::printf("  %zu rows, %.1f ms; the plan, after running:\n", r.row_count(), t * 1e3);
  indent(e.explain_last(), "    ");
}

}  // namespace

int main() {
  const char* path = "u10_group_bench.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 8192);  // every page stays cached
  Catalog cat(pool);
  Executor e(cat);
  std::mt19937_64 rng(1005);

  // 20,000 customers in 12 cities, the first cities far more populous; 200,000
  // orders, a few customers ordering far more often than most, over 365 days.
  const char* cities[] = {"Cairo", "Lagos", "Lima", "Oslo", "Pune", "Quito",
                          "Riga", "Seoul", "Tunis", "Turin", "Vigo", "Zagreb"};
  e.run_sql("CREATE TABLE customers (id INT64 NOT NULL, name TEXT, city TEXT)");
  for (int i = 0; i < 20000; ++i) {
    const std::uint64_t u = rng() % 1000;
    const int city = static_cast<int>(u * u * 12 / 1000000);  // skewed toward the first cities
    e.run_sql("INSERT INTO customers VALUES (" + std::to_string(i) + ", 'name_" + std::to_string(i) +
              "', '" + cities[city] + "')");
  }
  e.run_sql("CREATE TABLE orders (oid INT64 NOT NULL, customer INT64, cents INT64, day INT32)");
  for (int i = 0; i < 200000; ++i) {
    const std::uint64_t u = rng() % 20000;
    const std::uint64_t customer = (u * u) / 20000;  // low ids order more often
    const std::uint64_t cents = 100 + (rng() % 1000) * (rng() % 1000) / 10;  // mostly small
    e.run_sql("INSERT INTO orders VALUES (" + std::to_string(i) + ", " + std::to_string(customer) +
              ", " + std::to_string(cents) + ", " + std::to_string(1 + rng() % 365) + ")");
  }

  std::printf("\nan analytics session against 20000 customers and 200000 orders (median of %d runs):\n", kReps);
  session(e, "SELECT COUNT(*), SUM(cents), AVG(cents), MIN(cents), MAX(cents) FROM orders");
  session(e, "SELECT c.name, COUNT(*) AS orders, SUM(o.cents) AS spent FROM orders o "
             "JOIN customers c ON o.customer = c.id GROUP BY c.id, c.name ORDER BY spent DESC LIMIT 5");
  session(e, "SELECT c.city, COUNT(*) AS orders, SUM(o.cents) AS revenue, AVG(o.cents) AS avg_order "
             "FROM orders o JOIN customers c ON o.customer = c.id GROUP BY c.city ORDER BY revenue DESC");
  session(e, "SELECT day, COUNT(*) AS n FROM orders GROUP BY day ORDER BY n DESC, day LIMIT 3");
  session(e, "SELECT customer, COUNT(*) AS n FROM orders GROUP BY customer HAVING COUNT(*) >= 250 "
             "ORDER BY n DESC");
  ::unlink(path);

  // ---- the cardinality experiment: 2,000,000 rows, 10 to 1,000,000 groups ----
  const std::size_t kRows = 2'000'000;
  const OutSchema schema = {{"t", {"k", ColType::kInt64, false}}, {"t", {"v", ColType::kInt64, false}}};
  std::vector<Row> rows(kRows);
  const auto fill = [&](std::size_t groups, bool clustered) {
    // every group gets kRows / groups rows; shuffled, or each group's rows together
    std::vector<std::int64_t> keys(kRows);
    for (std::size_t i = 0; i < kRows; ++i) keys[i] = static_cast<std::int64_t>(i % groups);
    if (clustered) std::sort(keys.begin(), keys.end());
    else std::shuffle(keys.begin(), keys.end(), rng);
    for (std::size_t i = 0; i < kRows; ++i)
      rows[i] = {Field::Int64(keys[i]), Field::Int64(static_cast<std::int64_t>(rng() % 1000))};
  };
  const int kCliffReps = 7;
  const auto group = [&](std::size_t groups, bool clustered, const char* label) {
    fill(groups, clustered);
    std::size_t bytes = 0, n = 0;
    std::string detail;
    const std::vector<double> t = times_s([&] {
      HashAggregate agg(std::make_unique<RowsScan>(schema, rows), {0},
                        {{AggFunc::kCount, true, 0, ColType::kInt64},
                         {AggFunc::kSum, false, 1, ColType::kInt64}},
                        "GROUP BY k: COUNT(*), SUM(v)");
      agg.open();
      Row r;
      n = 0;
      while (agg.next(r)) ++n;
      agg.close();
      do_not_optimize(r);
      bytes = agg.bytes();
      detail = agg.detail();
    }, kCliffReps);
    REQUIRE(n == groups);
    const std::string probe_text = detail.substr(detail.rfind(' ') + 1);
    const auto ns = [&](double s) { return s * 1e9 / static_cast<double>(kRows); };
    std::printf("  %9zu %9zu %10s %9.0f B %8.1f ms %8.1f  (%5.1f-%5.1f) %6s%s\n", groups,
                kRows / groups, size_text(bytes).c_str(),
                static_cast<double>(bytes) / static_cast<double>(groups), t[kCliffReps / 2] * 1e3,
                ns(t[kCliffReps / 2]), ns(t.front()), ns(t.back()), probe_text.c_str(), label);
  };

  std::printf("\nthe cardinality experiment: 2000000 in-memory rows, GROUP BY k: COUNT(*), SUM(v)\n");
  std::printf("(sizeof: Field %zu, Row %zu, AggState %zu bytes; median of %d runs, with fastest-slowest)\n\n",
              sizeof(Field), sizeof(Row), sizeof(AggState), kCliffReps);
  fill(10, false);
  const double scan = median_s([&] {
    RowsScan s(schema, rows);
    s.open();
    Row r;
    while (s.next(r)) {}
    s.close();
    do_not_optimize(r);
  });
  std::printf("  for scale: RowsScan alone, the input: %.1f ms, %.1f ns per row\n\n", scan * 1e3,
              scan * 1e9 / static_cast<double>(kRows));
  std::printf("  %9s %9s %10s %11s %11s %8s  %13s %6s\n", "groups", "rows each", "table",
              "per group", "time", "ns/row", "(range)", "probe");
  for (std::size_t g : {10, 100, 1000, 10000, 100000, 1000000}) group(g, false, "");
  group(1000000, true, "  <- same rows, each group's together");
  return 0;
}
