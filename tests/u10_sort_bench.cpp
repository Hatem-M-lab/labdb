// Challenge 10.3, measured: what ORDER BY costs, and what a LIMIT saves.
//
// First in memory, so that storage costs nothing: 1,000,000 rows of three
// fields, ordered by a random 64-bit key -- all of them, and only the top n
// for n from 10 to 100,000 -- next to std::sort of the bare keys, the floor
// for any sort of these values. Then through SQL, against 200,000 orders on
// disk: the ten largest orders, run by the plan the planner builds (a top-n
// sort) and by a hand-built plan that sorts everything and then stops after
// ten; and LIMIT with no ORDER BY, which only has to stop reading. Every
// figure is the median of 5 runs. The in-memory sort is also timed with its
// key prefixes switched off, every comparison going to the rows themselves.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "buffer_pool.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "operator.hpp"
#include "pager.hpp"
#include "parser.hpp"

using namespace labdb;
using labdb::test::do_not_optimize;
using labdb::test::now_s;

namespace {

constexpr int kReps = 5;

// Median seconds of `run`, which must return `expect` rows every time.
template <class F>
double median_s(F&& run, std::size_t expect) {
  std::vector<double> t;
  for (int r = 0; r < kReps; ++r) {
    const double t0 = now_s();
    const std::size_t n = run();
    t.push_back(now_s() - t0);
    REQUIRE(n == expect);
  }
  std::sort(t.begin(), t.end());
  return t[t.size() / 2];
}

// Drain an operator tree, counting rows; the last row is kept observable.
std::size_t drain(Operator& op) {
  op.open();
  Row r;
  std::size_t n = 0;
  while (op.next(r)) ++n;
  op.close();
  do_not_optimize(r);
  return n;
}

}  // namespace

int main() {
  // ---- 1,000,000 in-memory rows: (id, key, payload) ----
  const std::size_t kRows = 1'000'000;
  std::mt19937_64 rng(103);
  std::vector<Row> rows;
  rows.reserve(kRows);
  for (std::size_t i = 0; i < kRows; ++i)
    rows.push_back({Field::Int64(static_cast<std::int64_t>(i)),
                    Field::Int64(static_cast<std::int64_t>(rng() >> 1)),
                    Field::Int32(static_cast<std::int32_t>(rng() % 1000))});
  const OutSchema schema = {{"t", {"id", ColType::kInt64, false}},
                            {"t", {"key", ColType::kInt64, false}},
                            {"t", {"payload", ColType::kInt32, false}}};

  std::printf("\nordering 1,000,000 in-memory rows by a random 64-bit key (median of %d runs)\n\n", kReps);

  std::vector<std::int64_t> keys(kRows);
  const double floor = median_s(
      [&] {
        for (std::size_t i = 0; i < kRows; ++i) keys[i] = rows[i][1].i64;
        std::sort(keys.begin(), keys.end());
        do_not_optimize(keys[0]);
        return keys.size();
      },
      kRows);
  const double scan = median_s(
      [&] {
        RowsScan s(schema, rows);
        return drain(s);
      },
      kRows);
  std::printf("  for scale: std::sort of the bare keys (8 MB of int64)  %7.1f ms\n", floor * 1e3);
  std::printf("             RowsScan alone, the Sort's input          %7.1f ms\n\n", scan * 1e3);

  // One Sort, timed in its two phases: open() reads every input row and
  // orders the entries; next() hands the rows out in that order.
  struct Timed {
    double open, next, total;
    std::string detail;
  };
  const auto run = [&](bool prefixes, std::optional<std::size_t> n) {
    std::vector<double> to, tn, tt;
    std::string detail;
    for (int r = 0; r < kReps; ++r) {
      Sort s(std::make_unique<RowsScan>(schema, rows), {{1, false}}, n, "key", prefixes);
      const double t0 = now_s();
      s.open();
      const double t1 = now_s();
      Row row;
      std::size_t k = 0;
      while (s.next(row)) ++k;
      const double t2 = now_s();
      s.close();
      do_not_optimize(row);
      REQUIRE(k == (n ? std::min(*n, kRows) : kRows));
      to.push_back(t1 - t0);
      tn.push_back(t2 - t1);
      tt.push_back(t2 - t0);
      detail = s.detail();
    }
    for (auto* v : {&to, &tn, &tt}) std::sort(v->begin(), v->end());
    return Timed{to[kReps / 2], tn[kReps / 2], tt[kReps / 2], detail};
  };
  std::printf("  %-24s %12s %14s %10s\n", "ORDER BY key", "open()", "next() x all", "total");
  const Timed slow = run(false, std::nullopt);
  const Timed full = run(true, std::nullopt);
  for (const auto& [label, t] : {std::pair{"Sort, no prefixes", slow}, std::pair{"Sort", full}})
    std::printf("  %-24s %9.1f ms %11.1f ms %7.1f ms\n", label, t.open * 1e3, t.next * 1e3,
                t.total * 1e3);

  // ORDER BY key LIMIT n. For input in random order, the row read i-th
  // (i > n) is among the best n so far with probability n/i, so the expected
  // number of rows that displace a held one is n (H_N - H_n), H the harmonic
  // numbers -- about n ln(N/n).
  const auto harmonic = [](std::size_t m) {
    double h = 0;
    for (std::size_t i = 1; i <= m; ++i) h += 1.0 / static_cast<double>(i);
    return h;
  };
  std::printf("\n  %-24s %12s %14s %14s %14s\n", "ORDER BY key LIMIT n", "total", "vs full sort",
              "replacements", "n(H_N - H_n)");
  for (std::size_t n : {10, 100, 1000, 10000, 100000}) {
    const Timed t = run(true, n);
    const std::size_t c = t.detail.rfind(", ");
    const std::string replaced = t.detail.substr(c + 2, t.detail.find(' ', c + 2) - c - 2);
    char label[64];
    std::snprintf(label, sizeof label, "top %zu", n);
    std::printf("  %-24s %9.1f ms %13.1fx %14s %14.0f\n", label, t.total * 1e3, full.total / t.total,
                replaced.c_str(), static_cast<double>(n) * (harmonic(kRows) - harmonic(n)));
  }

  // ---- 200,000 orders on disk, through SQL ----
  const char* path = "u10_sort_bench.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 4096);  // every page stays cached
  Catalog cat(pool);
  Executor e(cat);
  e.run_sql("CREATE TABLE orders (oid INT64 NOT NULL, customer INT64, cents INT64)");
  for (int i = 0; i < 200000; ++i)
    e.run_sql("INSERT INTO orders VALUES (" + std::to_string(i) + ", " +
              std::to_string(rng() % 20000) + ", " + std::to_string(rng() % 1000000) + ")");

  const std::string top_sql = "SELECT oid, cents FROM orders ORDER BY cents DESC LIMIT 10";
  std::printf("\nagainst 200,000 orders on disk (all pages cached), median of %d runs:\n\n", kReps);
  std::printf("  SQL> %s\n", top_sql.c_str());
  const double top = median_s([&] { return e.run_sql(top_sql).row_count(); }, 10);
  std::printf("  the planner's plan, after running:\n");
  {
    std::string plan = e.explain_last(), line;
    for (char c : plan) {
      line += c;
      if (c == '\n') { std::printf("    %s", line.c_str()); line.clear(); }
    }
  }
  // The same query, planned by hand the way it would run without top-n:
  // sort all 200,000 rows, then stop after ten.
  const TableRef orders{"orders", ""};
  const double sort_all = median_s(
      [&] {
        Project p(std::make_unique<Limit>(std::make_unique<Sort>(std::make_unique<SeqScan>(cat, orders),
                                                                 std::vector<SortKey>{{2, true}},
                                                                 std::nullopt, "cents DESC"),
                                          10),
                  {0, 2}, "oid, cents");
        return drain(p);
      },
      10);
  const double scan_only = median_s(
      [&] {
        SeqScan s(cat, orders);
        return drain(s);
      },
      200000);
  std::printf("\n  %-46s %8.1f ms\n", "top-10 sort (the planner's plan)", top * 1e3);
  std::printf("  %-46s %8.1f ms\n", "full sort, then Limit 10 (built by hand)", sort_all * 1e3);
  std::printf("  %-46s %8.1f ms\n", "for scale: SeqScan orders alone", scan_only * 1e3);

  const std::string lim_sql = "SELECT oid, cents FROM orders LIMIT 10";
  const double lim = median_s([&] { return e.run_sql(lim_sql).row_count(); }, 10);
  std::printf("\n  SQL> %s   (%.1f us)\n", lim_sql.c_str(), lim * 1e6);
  std::printf("  plan, after running:\n");
  {
    std::string plan = e.explain_last(), line;
    for (char c : plan) {
      line += c;
      if (c == '\n') { std::printf("    %s", line.c_str()); line.clear(); }
    }
  }
  ::unlink(path);
  return 0;
}
