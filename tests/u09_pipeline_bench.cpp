// Challenge 9.1, measured: what the iterator model costs per row.
//
// One question -- "the oid of every order with cents < 50000" -- answered
// five ways over the same 1,000,000 rows held in memory, so that storage costs
// nothing and only the way rows are processed is on the clock. Then the same
// pipeline over a real 200,000-row table, to put that cost next to the cost of
// reading a row out of a page. Last, the cost that is paid once per query
// rather than per row: parsing and planning, measured on a point lookup,
// whose whole answer is one row. Every figure is the median of 7 runs.
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

constexpr int kReps = 7;

// Median nanoseconds per input row of `run`, which returns its result count.
template <class F>
double ns_per_row(F&& run, std::size_t rows_in, std::size_t& count) {
  std::vector<double> t;
  for (int r = 0; r < kReps; ++r) {
    const double t0 = now_s();
    count = run();
    t.push_back((now_s() - t0) * 1e9 / static_cast<double>(rows_in));
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
  // ---- 1,000,000 in-memory rows: (oid, customer, cents) ----
  const int kRows = 1'000'000;
  std::mt19937_64 rng(91);
  std::vector<Row> rows;
  rows.reserve(kRows);
  for (int i = 0; i < kRows; ++i)
    rows.push_back({Field::Int64(i), Field::Int32(static_cast<std::int32_t>(rng() % 20000)),
                    Field::Int32(static_cast<std::int32_t>(rng() % 100000))});
  const OutSchema schema = {{"orders", {"oid", ColType::kInt64, false}},
                            {"orders", {"customer", ColType::kInt32, false}},
                            {"orders", {"cents", ColType::kInt32, false}}};
  BoundComparison lt;  // cents < 50000, bound once
  lt.left = 2;
  lt.op = TokenType::kLt;
  lt.literal = Field::Int64(50000);

  std::size_t n1 = 0, n2 = 0, n3 = 0, n4 = 0, n5 = 0;

  // (1) a hand-written loop over the rows where they lie
  const double hand = ns_per_row(
      [&] {
        std::size_t c = 0;
        std::int64_t sink = 0;
        for (const Row& r : rows)
          if (r[2].i64 < 50000) { sink = r[0].i64; ++c; }
        do_not_optimize(sink);
        return c;
      },
      kRows, n1);

  // (2) the same loop, but each row copied into a buffer first -- which is
  // what next(Row& out) does: every operator hands its parent a row by
  // filling the parent's buffer.
  const double hand_copy = ns_per_row(
      [&] {
        std::size_t c = 0;
        std::int64_t sink = 0;
        Row buf;
        for (const Row& r : rows) {
          buf = r;
          if (buf[2].i64 < 50000) { sink = buf[0].i64; ++c; }
        }
        do_not_optimize(sink);
        return c;
      },
      kRows, n2);

  // (3) the operators: a scan alone, then a filter over it, then a
  // projection over that -- the three-operator pipeline of a one-table query.
  const double scan_only = ns_per_row(
      [&] {
        RowsScan s(schema, rows);
        return drain(s);
      },
      kRows, n3);
  const double scan_filter = ns_per_row(
      [&] {
        Filter f(std::make_unique<RowsScan>(schema, rows), {lt}, "cents < 50000");
        return drain(f);
      },
      kRows, n4);
  const double pipeline = ns_per_row(
      [&] {
        Project p(std::make_unique<Filter>(std::make_unique<RowsScan>(schema, rows),
                                           std::vector<BoundComparison>{lt}, "cents < 50000"),
                  {0}, "oid");
        return drain(p);
      },
      kRows, n5);
  REQUIRE(n1 == n2 && n2 == n4 && n4 == n5 && n3 == static_cast<std::size_t>(kRows));

  std::printf("the pipeline in isolation: %d rows in memory; the oid of every row with "
              "cents < 50000 (%zu rows); median of %d runs\n\n", kRows, n5, kReps);
  std::printf("  how the rows are processed                         ns/row\n");
  std::printf("  hand-written loop, rows read where they lie          %5.1f\n", hand);
  std::printf("  hand-written loop, each row first copied to a buffer %5.1f\n", hand_copy);
  std::printf("  RowsScan                        (1 operator)         %5.1f\n", scan_only);
  std::printf("  Filter <- RowsScan              (2 operators)        %5.1f\n", scan_filter);
  std::printf("  Project <- Filter <- RowsScan   (3 operators)        %5.1f\n", pipeline);

  // ---- the same pipeline over a real table, every page in the pool ----
  const char* path = "u09_pipeline_bench.db";
  ::unlink(path);
  {
    Pager pager(path);
    BufferPool pool(pager, 8192);
    Catalog cat(pool);
    Executor e(cat);
    e.run_sql("CREATE TABLE orders (oid INT64 NOT NULL, customer INT32 NOT NULL, cents INT32 NOT NULL)");
    const int kTable = 200'000;
    e.run_sql("BEGIN");  // Unit 13: one transaction, so one commit, for the whole load
    for (int i = 0; i < kTable; ++i)
      e.run_sql("INSERT INTO orders VALUES (" + std::to_string(i) + ", " +
                std::to_string(rows[i][1].i64) + ", " + std::to_string(rows[i][2].i64) + ")");
    e.run_sql("COMMIT");

    std::size_t m1 = 0, m2 = 0;
    const double seq = ns_per_row(
        [&] {
          SeqScan s(cat, TableRef{"orders", ""});
          return drain(s);
        },
        kTable, m1);
    const double sql = ns_per_row(
        [&] { return e.run_sql("SELECT oid FROM orders WHERE cents < 50000").row_count(); },
        kTable, m2);
    REQUIRE(m1 == static_cast<std::size_t>(kTable));

    std::printf("\nthe same pipeline over a real table: %d rows on disk, every page in the "
                "buffer pool; median of %d runs\n\n", kTable, kReps);
    std::printf("  how the rows are processed                         ns/row\n");
    std::printf("  SeqScan          (tree cursor, heap fetch, decode)   %5.1f\n", seq);
    std::printf("  SELECT oid FROM orders WHERE cents < 50000           %5.1f   "
                "(%zu rows: parse, plan, scan, filter, project, collect)\n", sql, m2);

    // ---- per query: a point lookup, taken apart ----
    const int kQ = 100'000;
    std::vector<std::string> qs;
    std::mt19937_64 qr(8);
    for (int k = 0; k < kQ; ++k)
      qs.push_back("SELECT customer FROM orders WHERE oid = " + std::to_string(qr() % kTable));
    std::size_t c1 = 0, c2 = 0, c3 = 0;
    const double parse_only = ns_per_row(
        [&] {
          std::size_t n = 0;
          for (const std::string& q : qs) {
            Parser p(q);
            n += p.parse_single().select.where.size();
          }
          return n;
        },
        kQ, c1);
    const double parse_plan = ns_per_row(
        [&] {
          std::size_t n = 0;
          for (const std::string& q : qs) {
            Parser p(q);
            const Statement st = p.parse_single();
            Executor::Plan pl = e.plan(st.select);
            n += pl.column_names.size();
          }
          return n;
        },
        kQ, c2);
    const double whole = ns_per_row(
        [&] {
          std::size_t n = 0;
          for (const std::string& q : qs) n += e.run_sql(q).row_count();
          return n;
        },
        kQ, c3);
    REQUIRE(c1 == static_cast<std::size_t>(kQ) && c3 == static_cast<std::size_t>(kQ));

    std::printf("\nper query, not per row: %d point lookups "
                "(SELECT customer FROM orders WHERE oid = ?); median of %d runs\n\n", kQ, kReps);
    std::printf("  stage                                             us/query\n");
    std::printf("  parse                                               %5.2f\n", parse_only / 1000);
    std::printf("  plan   (build the operator tree, then discard it)   %5.2f\n",
                (parse_plan - parse_only) / 1000);
    std::printf("  run    (index lookup, heap fetch, project, collect) %5.2f\n",
                (whole - parse_plan) / 1000);
    std::printf("  the whole query                                     %5.2f\n", whole / 1000);
  }
  ::unlink(path);
  return 0;
}
