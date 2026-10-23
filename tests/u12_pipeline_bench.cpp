// Challenge 12.3: what batches change, per row.
//
// Unit 9 measured its pipeline over 1,000,000 rows held in memory -- the oid
// of every row with cents < 50000 -- at 38.0 ns a row through three operators
// on that unit's machine, and Unit 12 promised to come back to it. Here it is
// again, on this unit's machine, both ways: rows through the row path, and
// the same values, held as columns, through the batch path. The same seed,
// the same rows.
//
// Then two curves. The first is the cost of the filter as the share of rows
// that pass it goes from none to all: a row-at-a-time filter branches on every
// row's answer, a batch filter writes every position and moves its count on
// only for the rows that pass, so it does not branch on the answer. The second
// is the batch size, from 1 row to the whole input, in memory and over a table
// on disk. Within each table, everything compared is timed in turn, round
// after round (tests/interleave.hpp). Medians of 11 rounds.
//
//   u12_pipeline_bench count PLAN N   runs one plan of the first table N times
//                                     and prints nothing, for counting
//                                     (scripts/u12_pipeline_counts.sh)
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "buffer_pool.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "interleave.hpp"
#include "operator.hpp"
#include "pager.hpp"

using namespace labdb;
using labdb::test::do_not_optimize;
using labdb::test::interleaved;
using labdb::test::Timing;

namespace {

constexpr int kRounds = 11;
constexpr std::size_t kRows = 1'000'000;

std::size_t drain_rows(Operator& op) {
  op.open();
  Row r;
  std::size_t n = 0;
  while (op.next(r)) ++n;
  op.close();
  do_not_optimize(r);
  return n;
}

std::size_t drain_batches(Operator& op, std::size_t batch) {
  op.set_batch_rows(batch);
  op.open();
  Batch b;
  std::size_t n = 0;
  while (op.next_batch(b)) {
    n += b.count();
    do_not_optimize(b.cols[0].ints[b.row(0)]);
  }
  op.close();
  return n;
}

double ns(const Timing& t, std::size_t rows) { return t.median * 1e9 / static_cast<double>(rows); }

}  // namespace

int main(int argc, char** argv) {
  // ---- Unit 9's 1,000,000 rows (oid, customer, cents), as rows and as columns ----
  std::mt19937_64 rng(91);
  std::vector<Row> rows;
  rows.reserve(kRows);
  std::vector<ColumnVector> cols(3);
  for (ColumnVector& c : cols) c.type = ColType::kInt64;
  cols[1].type = cols[2].type = ColType::kInt32;
  for (std::size_t i = 0; i < kRows; ++i) {
    const auto customer = static_cast<std::int32_t>(rng() % 20000);
    const auto cents = static_cast<std::int32_t>(rng() % 100000);
    rows.push_back({Field::Int64(static_cast<std::int64_t>(i)), Field::Int32(customer), Field::Int32(cents)});
    cols[0].push(rows.back()[0]);
    cols[1].push(rows.back()[1]);
    cols[2].push(rows.back()[2]);
  }
  const OutSchema schema = {{"orders", {"oid", ColType::kInt64, false}},
                            {"orders", {"customer", ColType::kInt32, false}},
                            {"orders", {"cents", ColType::kInt32, false}}};
  const auto below = [](std::int64_t x) {
    BoundComparison lt;
    lt.left = 2;
    lt.op = TokenType::kLt;
    lt.literal = Field::Int64(x);
    return lt;
  };
  const auto scan = [&](bool batches) -> OpPtr {
    return batches ? OpPtr(std::make_unique<ColumnsScan>(schema, cols, kRows))
                   : OpPtr(std::make_unique<RowsScan>(schema, rows));
  };
  const auto filter_over = [&](bool batches, std::int64_t x) -> OpPtr {
    return std::make_unique<Filter>(scan(batches), std::vector<BoundComparison>{below(x)}, "cents < x");
  };
  const auto pipeline = [&](bool batches) -> OpPtr {
    return std::make_unique<Project>(filter_over(batches, 50000), std::vector<std::size_t>{0}, "oid");
  };
  const auto loop_rows = [&] {  // the hand-written loops of the first table
    std::size_t c = 0;
    std::int64_t sink = 0;
    for (const Row& r : rows)
      if (r[2].i64 < 50000) sink = r[0].i64, ++c;
    do_not_optimize(sink);
    return c;
  };
  const auto loop_columns = [&] {
    std::size_t c = 0;
    std::int64_t sink = 0;
    const std::int64_t* cents = cols[2].ints.data();
    const std::int64_t* oid = cols[0].ints.data();
    for (std::size_t i = 0; i < kRows; ++i)
      if (cents[i] < 50000) sink = oid[i], ++c;
    do_not_optimize(sink);
    return c;
  };
  if (argc == 4 && std::string(argv[1]) == "count") {
    const std::string plan = argv[2];  // rows-PART or batches-PART; PART: loop, scan, filter, project
    const long n = std::atol(argv[3]);
    const bool b = plan.rfind("batches-", 0) == 0;
    const std::string part = plan.substr(plan.find('-') + 1);
    for (long i = 0; i < n; ++i) {
      if (part == "loop") {
        do_not_optimize(b ? loop_columns() : loop_rows());
        continue;
      }
      OpPtr op = part == "scan" ? scan(b) : part == "filter" ? filter_over(b, 50000) : pipeline(b);
      do_not_optimize(b ? drain_batches(*op, kDefaultBatchRows) : drain_rows(*op));
    }
    return 0;
  }

  // A run: build the plan, drain it, check its count.
  const auto run = [](std::function<OpPtr()> make, std::size_t batch, std::size_t want) {
    return [make, batch, want] {
      OpPtr op = make();
      const std::size_t n = batch ? drain_batches(*op, batch) : drain_rows(*op);
      REQUIRE(n == want);
    };
  };

  std::size_t want = 0;
  for (const Row& r : rows) want += r[2].i64 < 50000;
  {
    std::vector<std::function<void()>> runs;
    runs.push_back([&] { REQUIRE(loop_rows() == want); });
    runs.push_back([&] { REQUIRE(loop_columns() == want); });
    for (bool b : {false, true}) {
      const std::size_t batch = b ? kDefaultBatchRows : 0;
      runs.push_back(run([&, b] { return scan(b); }, batch, kRows));
      runs.push_back(run([&, b] { return filter_over(b, 50000); }, batch, want));
      runs.push_back(run([&, b] { return pipeline(b); }, batch, want));
    }
    const std::vector<Timing> t = interleaved(runs, kRounds);
    std::printf("\nUnit 9's pipeline, both ways: %zu rows in memory; the oid of every row with cents < 50000 "
                "(%zu rows); median of %d rounds\n\n", kRows, want, kRounds);
    std::printf("  how the rows are processed                    ns/row:  a row at a time   in batches of 1024\n");
    std::printf("  a hand-written loop over the data where it lies               %5.1f              %5.1f\n",
                ns(t[0], kRows), ns(t[1], kRows));
    std::printf("  the scan                        (1 operator)                  %5.1f              %5.1f\n",
                ns(t[2], kRows), ns(t[5], kRows));
    std::printf("  Filter <- the scan              (2 operators)                 %5.1f              %5.1f\n",
                ns(t[3], kRows), ns(t[6], kRows));
    std::printf("  Project <- Filter <- the scan   (3 operators)                 %5.1f              %5.1f\n",
                ns(t[4], kRows), ns(t[7], kRows));
    std::printf("  (the scan: RowsScan over Rows, a row at a time; ColumnsScan over columns, in batches)\n");
  }

  // ---- the filter's cost against the share of rows it keeps ----
  {
    const std::int64_t xs[] = {0, 1000, 10000, 25000, 50000, 75000, 90000, 99000, 100000};
    std::vector<std::function<void()>> runs;
    std::vector<std::size_t> kept;
    for (std::int64_t x : xs) {
      std::size_t k = 0;
      for (const Row& r : rows) k += r[2].i64 < x;
      kept.push_back(k);
      runs.push_back(run([&, x] { return filter_over(false, x); }, 0, k));
      runs.push_back(run([&, x] { return filter_over(true, x); }, kDefaultBatchRows, k));
    }
    const std::vector<Timing> t = interleaved(runs, kRounds);
    std::printf("\nFilter <- the scan, cents < X, as the share of rows that pass grows (ns/row)\n\n");
    std::printf("      X   pass   a row at a time   in batches\n");
    for (std::size_t i = 0; i < std::size(xs); ++i)
      std::printf("  %6lld  %4.0f%%  %15.1f  %11.1f\n", static_cast<long long>(xs[i]),
                  100.0 * static_cast<double>(kept[i]) / kRows, ns(t[2 * i], kRows), ns(t[2 * i + 1], kRows));
  }

  // ---- the batch size ----
  {
    const std::size_t sizes[] = {1, 2, 4, 16, 64, 256, 1024, 4096, 16384, 65536, 262144, kRows};
    std::vector<std::function<void()>> runs;
    for (std::size_t size : sizes) runs.push_back(run([&] { return pipeline(true); }, size, want));
    runs.push_back(run([&] { return pipeline(false); }, 0, want));
    const std::vector<Timing> t = interleaved(runs, kRounds);
    std::printf("\nProject <- Filter <- ColumnsScan, cents < 50000, by batch size (ns/row; the batch's columns, in bytes)\n\n");
    std::printf("     batch rows    ns/row      a batch holds\n");
    for (std::size_t i = 0; i < std::size(sizes); ++i) {
      const double bytes = static_cast<double>(sizes[i]) * (3 * 8 + 3 + 4);  // three value arrays, three null arrays, sel
      std::printf("  %13zu  %8.1f   %10.0f KB\n", sizes[i], ns(t[i], kRows), bytes / 1024);
    }
    std::printf("  (a row at a time, for comparison:  %.1f)\n", ns(t[std::size(sizes)], kRows));
  }

  // ---- the same curve over a table on disk ----
  const char* path = "u12_pipeline_bench.db";
  ::unlink(path);
  {
    Pager pager(path);
    BufferPool pool(pager, 8192);
    Catalog cat(pool);
    Executor e(cat);
    std::mt19937_64 r(1103);
    e.run_sql("CREATE TABLE events (id INT64 NOT NULL, v INT64, w INT64)");
    e.run_sql("BEGIN");  // Unit 13: one transaction, so one commit, for the whole load
    for (int i = 0; i < 200000; ++i)
      e.run_sql("INSERT INTO events VALUES (" + std::to_string(i) + ", " + std::to_string(r() % 1000000) + ", " +
                std::to_string(r() % 1000) + ")");
    e.run_sql("COMMIT");
  }
  {
    Pager pager(path);
    BufferPool pool(pager, 8192);
    Catalog cat(pool);
    Executor e(cat);
    const std::string sql = "SELECT SUM(w) FROM events WHERE v < 500000";
    const std::size_t sizes[] = {1, 4, 16, 64, 256, 1024, 4096, 16384, 65536};
    std::vector<std::function<void()>> runs;
    const auto with = [&](std::size_t size, bool copy) {
      return [&e, &cat, &sql, size, copy] {
        e.set_batch_rows(size);
        cat.set_copy_pages(copy);
        e.run_sql(sql);
        cat.set_copy_pages(false);
      };
    };
    runs.push_back(with(0, true));
    runs.push_back(with(0, false));
    for (std::size_t size : sizes) runs.push_back(with(size, false));
    const std::vector<Timing> t = interleaved(runs, kRounds);
    e.set_batch_rows(Executor::kChoose);
    std::printf("\n%s -- 200000 rows on disk, every page in the pool, by batch size (ns per row scanned)\n\n",
                sql.c_str());
    std::printf("     batch rows    ns/row\n");
    std::printf("  a row at a time, pages copied (Unit 11)  %6.1f\n", ns(t[0], 200000));
    std::printf("  a row at a time, pages read in place     %6.1f\n", ns(t[1], 200000));
    for (std::size_t i = 0; i < std::size(sizes); ++i)
      std::printf("  %13zu  %8.1f\n", sizes[i], ns(t[2 + i], 200000));
  }
  ::unlink(path);
  return 0;
}
