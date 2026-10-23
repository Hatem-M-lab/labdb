// Challenge 11.3: when an index loses.
//
// First the calibration. The planner compares plans by estimated time, built
// from a handful of per-row costs, and here each one is measured on its own:
// a row of a scan, a row fetched through a secondary index (its page in the
// buffer pool, and not), one index search, a row into a join's hash table and
// a row probed against it, a pair compared by a nested loop. The constants in
// src/executor.cpp are this program's numbers, rounded.
//
// Then the race. SELECT SUM(w) FROM events WHERE v < X, over 200,000 rows with
// an index on v, for X from almost nothing to everything: the same query
// through a full scan and through the index, timed, with the plan the planner
// chose -- once with a buffer pool that holds the whole table, and once with a
// pool too small for its heap. Timings are medians of 5 runs (15 for the
// calibration), printed with the fastest and slowest where the noise matters.
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

using namespace labdb;
using labdb::test::now_s;

namespace {

struct Timing {
  double median, lo, hi;
};

template <class F>
Timing timed(F&& run, int reps) {
  std::vector<double> t;
  for (int r = 0; r < reps; ++r) {
    const double t0 = now_s();
    run();
    t.push_back(now_s() - t0);
  }
  std::sort(t.begin(), t.end());
  return {t[t.size() / 2], t.front(), t.back()};
}

std::size_t drain(Operator& op) {
  op.open();
  Row r;
  std::size_t n = 0;
  while (op.next(r)) ++n;
  op.close();
  return n;
}

constexpr int kRows = 200000;
constexpr std::int64_t kDomain = 1000000;  // v is uniform in [0, kDomain)

}  // namespace

int main() {
  const char* path = "u11_cost_bench.db";
  ::unlink(path);
  std::mt19937_64 rng(1103);
  {
    Pager pager(path);
    BufferPool pool(pager, 8192);
    Catalog cat(pool);
    Executor e(cat);
    e.run_sql("CREATE TABLE events (id INT64 NOT NULL, v INT64, w INT64)");
    e.run_sql("BEGIN");  // Unit 13: one transaction, so one commit, for the whole load
    for (int i = 0; i < kRows; ++i)
      e.run_sql("INSERT INTO events VALUES (" + std::to_string(i) + ", " +
                std::to_string(rng() % kDomain) + ", " + std::to_string(rng() % 1000) + ")");
    e.run_sql("COMMIT");
    e.run_sql("CREATE INDEX ON events (v)");
  }

  // ------------------------------------------------------ calibration
  double seq = 0, rnd = 0, miss = 0, seek = 0, build = 0, probe = 0, pair = 0;
  std::uint32_t heap_pages = 0;
  {
    Pager pager(path);
    BufferPool pool(pager, 8192);
    Catalog cat(pool);
    const TableInfo info = *cat.find_table("events");
    heap_pages = info.heap_pages;
    const OutSchema schema = table_schema(info.schema, TableRef{"events", ""});
    std::printf("\nevents: %d rows; the heap is %u pages, the file %u pages\n", kRows, heap_pages,
                pool.page_count());
    std::printf("\ncalibration: one step of a plan at a time (median of 15 runs, per row)\n");
    SeqScan scan(cat, TableRef{"events", ""}, schema);
    drain(scan);  // warm the pool
    const Timing ts = timed([&] { drain(scan); }, 15);
    seq = ts.median * 1e9 / kRows;
    IndexScan through(cat, TableRef{"events", ""}, schema, 1, 0, ~Key{0}, "every v");
    drain(through);
    const Timing ti = timed([&] { drain(through); }, 15);
    rnd = ti.median * 1e9 / kRows;
    std::printf("  a row of a SeqScan: the next key, its heap page, decoded     %6.0f ns  (%.0f-%.0f)\n",
                seq, ts.lo * 1e9 / kRows, ts.hi * 1e9 / kRows);
    std::printf("  a row through the index on v, in v's order, page in the pool %6.0f ns  (%.0f-%.0f)\n",
                rnd, ti.lo * 1e9 / kRows, ti.hi * 1e9 / kRows);

    // one search: seek to a key no row holds (v is drawn below kDomain), so
    // the search is all there is
    IndexCursor cur = cat.open_index("events", 1);
    std::vector<Key> probes;
    for (int i = 0; i < 100000; ++i)
      probes.push_back(order_key(Field::Int64(kDomain + static_cast<std::int64_t>(rng() % kDomain))));
    std::shuffle(probes.begin(), probes.end(), rng);
    const Timing tk = timed([&] {
      Row r;
      for (Key k : probes) {
        cur.seek(k, k);
        cur.next(r);
      }
    }, 15);
    seek = tk.median * 1e9 / static_cast<double>(probes.size());
    std::printf("  one index search, root to leaf                               %6.0f ns  (%.0f-%.0f)\n",
                seek,
                tk.lo * 1e9 / static_cast<double>(probes.size()),
                tk.hi * 1e9 / static_cast<double>(probes.size()));
    // what the planner's estimate costs: counting entries through the leaves
    std::uint64_t counted = 0;
    const Timing tc = timed([&] { counted = cat.count_index("events", 1, 0, ~Key{0}, ~std::uint64_t{0}); }, 15);
    std::printf("  counting one entry of the index, for an estimate            %6.1f ns  (%.1f-%.1f)\n",
                tc.median * 1e9 / static_cast<double>(counted), tc.lo * 1e9 / static_cast<double>(counted),
                tc.hi * 1e9 / static_cast<double>(counted));
  }
  {
    // the same index walk through a pool that holds a sixth of the heap
    Pager pager(path);
    BufferPool pool(pager, 256);
    Catalog cat(pool);
    const OutSchema schema = table_schema(cat.schema_of("events"), TableRef{"events", ""});
    IndexScan through(cat, TableRef{"events", ""}, schema, 1, 0, ~Key{0}, "every v");
    drain(through);
    pool.reset_stats();
    const Timing tm = timed([&] { drain(through); }, 15);
    const double reads = static_cast<double>(pool.disk_reads()) / 15.0 / kRows;  // file reads per row
    const double per_row = tm.median * 1e9 / kRows;
    miss = (per_row - rnd) / reads;
    std::printf("  ... with a pool of 256 pages, %4.1f%% of rows read from the file %6.0f ns  (%.0f-%.0f)\n",
                reads * 100, per_row, tm.lo * 1e9 / kRows, tm.hi * 1e9 / kRows);
    std::printf("      so a read from the file adds                             %6.0f ns\n", miss);
  }
  {
    // hash join and nested loop, over rows in memory: the storage is not
    // what is measured here
    const OutSchema ks = {{"a", {"k", ColType::kInt64, false}}, {"a", {"x", ColType::kInt64, false}}};
    const OutSchema rs = {{"b", {"k", ColType::kInt64, false}}, {"b", {"y", ColType::kInt64, false}}};
    std::vector<Row> big, none, few;
    for (std::int64_t i = 0; i < kRows; ++i) big.push_back({Field::Int64(i), Field::Int64(i)});
    for (std::int64_t i = 0; i < 1000; ++i) few.push_back({Field::Int64(i * kRows), Field::Int64(i)});
    const auto join_ns = [&](const std::vector<Row>& l, const std::vector<Row>& r) {
      HashJoin hj(std::make_unique<RowsScan>(ks, l), std::make_unique<RowsScan>(rs, r), {{0, 0}}, {}, "a.k = b.k");
      return timed([&] { drain(hj); }, 15).median * 1e9;
    };
    RowsScan only(ks, big);
    const double input = timed([&] { drain(only); }, 15).median * 1e9 / kRows;
    build = join_ns(none, big) / kRows - input;  // build 200,000, probe nothing
    probe = join_ns(big, few) / kRows - input;   // probe 200,000 past 1,000, (almost) no match
    std::vector<Row> l2(big.begin(), big.begin() + 2000), r2;
    for (std::int64_t i = 0; i < 2000; ++i) r2.push_back({Field::Int64(-1 - i), Field::Int64(i)});
    NestedLoopJoin nl(std::make_unique<RowsScan>(ks, l2), std::make_unique<RowsScan>(rs, r2),
                      {BoundComparison{0, TokenType::kEq, false, false, true, 2, Field{}}}, "a.k = b.k");
    pair = timed([&] { drain(nl); }, 15).median * 1e9 / (2000.0 * 2000.0);
    std::printf("  a row into a join's hash table                               %6.0f ns\n", build);
    std::printf("  a row probed against it                                      %6.0f ns\n", probe);
    std::printf("  a pair of rows a nested loop compares                        %6.1f ns\n", pair);
  }
  std::printf("the planner's constants (src/executor.hpp): %.0f, %.0f, %.0f, %.0f, %.0f, %.0f, %.0f ns\n",
              cost::kSeqRow, cost::kRandomRow, cost::kMissRow, cost::kSeek, cost::kBuildRow,
              cost::kProbeRow, cost::kPairRow);
  std::printf("this run measured:                          %.0f, %.0f, %.0f, %.0f, %.0f, %.0f, %.0f ns\n",
              seq, rnd, miss, seek, build, probe, pair);

  // ------------------------------------------------------------ the race
  const double sels[] = {0.0001, 0.001, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 1.0};
  for (std::size_t frames : {std::size_t{8192}, std::size_t{256}}) {
    Pager pager(path);
    BufferPool pool(pager, frames);
    Catalog cat(pool);
    Executor e(cat);
    const double held = std::min(1.0, static_cast<double>(frames) / heap_pages);
    std::printf("\nSELECT SUM(w) FROM events WHERE v < X -- a pool of %zu pages, %s\n", frames,
                held >= 1 ? "the whole heap fits" : "a sixth of the heap fits");
    std::printf("  selectivity      rows    scan ms         index ms        faster  planner chose\n");
    double crossover = -1;
    double prev_sel = 0, prev_ratio = 0;
    for (double sel : sels) {
      const std::string sql = "SELECT SUM(w) FROM events WHERE v < " +
                              std::to_string(static_cast<std::int64_t>(sel * kDomain));
      std::size_t rows = 0;
      const auto time_with = [&](Executor::AccessPath a) {
        e.set_access_path(a);
        e.run_sql(sql);  // warm
        const Timing t = timed([&] { e.run_sql(sql); }, 5);
        const std::string plan = e.explain_last();
        const std::size_t at = plan.find(a == Executor::AccessPath::kScan ? "Filter" : "IndexScan");
        rows = std::stoul(plan.substr(plan.find("rows=", at) + 5));
        return t;
      };
      const Timing scan = time_with(Executor::AccessPath::kScan);
      const Timing index = time_with(Executor::AccessPath::kIndex);
      e.set_access_path(Executor::AccessPath::kAuto);
      e.run_sql(sql);
      const bool chose_index = e.explain_last().find("IndexScan") != std::string::npos;
      const double ratio = index.median / scan.median;
      if (crossover < 0 && ratio > 1 && prev_ratio > 0)  // where index time / scan time crosses 1
        crossover = prev_sel + (sel - prev_sel) * (1 - prev_ratio) / (ratio - prev_ratio);
      prev_sel = sel;
      prev_ratio = ratio;
      char sb[32];
      std::snprintf(sb, sizeof sb, "%g%%", sel * 100);
      std::printf("  %9s %9zu  %6.1f (%4.1f-%4.1f)  %6.1f (%5.1f-%5.1f)  %-6s  %s\n", sb, rows,
                  scan.median * 1e3, scan.lo * 1e3, scan.hi * 1e3, index.median * 1e3, index.lo * 1e3,
                  index.hi * 1e3, ratio < 1 ? "index" : "scan", chose_index ? "index" : "scan");
    }
    const double model = cost::kSeqRow / (cost::kRandomRow + (1 - held) * cost::kMissRow);
    std::printf("  the index stops winning at %.0f%% of the rows; the planner's constants put it at %.0f%%\n",
                crossover * 100, model * 100);
  }
  ::unlink(path);
  return 0;
}
