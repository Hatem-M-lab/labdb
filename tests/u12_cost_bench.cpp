// Challenge 12.6: when an index loses, now that a scanned row is cheap.
//
// Unit 11 found the break-even between reading a whole table and going
// through an index at about half the table, and traced it to the ratio of two
// per-row costs: a row fetched out of order through an index, and a row read
// in order by a scan. Unit 12 has made the second far cheaper than the first.
//
// First the calibration again, each per-row cost measured on its own, now as
// the engine runs -- in batches -- and, for the two rows the race turns on, as
// Unit 11 ran them (pages copied, a row at a time), in the same run. The
// constants in src/executor.hpp are this program's batch numbers, rounded.
//
// Then the race again: SELECT SUM(w) FROM events WHERE v < X, over 200,000
// rows with an index on v, the scan and the index each forced, at fifteen
// selectivities, with a pool that holds the heap and one that holds a sixth of
// it -- in batches, beside the plan the planner chose; and Unit 11's engine on
// the same queries, for its crossover on this machine. Between the two, where
// batches start to pay: the rows from which the executor runs a plan in
// batches (cost::kBatchFrom). What is compared is
// timed in turn, round after round (tests/interleave.hpp): medians of 7 rounds
// (15 for the calibration), with the fastest and slowest where the noise
// matters.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
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
using labdb::test::interleaved;
using labdb::test::Timing;

namespace {

constexpr int kRaceRounds = 7;

// One thing timed on its own: a warm run, then `reps` runs.
Timing timed(std::function<void()> run, int reps) { return interleaved({std::move(run)}, reps)[0]; }

// Every row of a plan, in batches of `batch` rows (0: a row at a time).
std::size_t drain(Operator& op, std::size_t batch) {
  op.set_batch_rows(batch);
  op.open();
  std::size_t n = 0;
  if (batch == 0) {
    Row r;
    while (op.next(r)) ++n;
  } else {
    Batch b;
    while (op.next_batch(b)) n += b.count();
  }
  op.close();
  return n;
}

constexpr int kRows = 200000;
constexpr std::int64_t kDomain = 1000000;  // v is uniform in [0, kDomain)

ColumnVector ints(const std::vector<std::int64_t>& v) {
  ColumnVector c;
  c.type = ColType::kInt64;
  c.ints = v;
  c.nulls.assign(v.size(), 0);
  return c;
}

}  // namespace

int main() {
  const char* path = "u12_cost_bench.db";
  ::unlink(path);
  std::mt19937_64 rng(1103);
  {
    Pager pager(path);
    BufferPool pool(pager, 8192);
    Catalog cat(pool);
    Executor e(cat);
    e.run_sql("CREATE TABLE events (id INT64 NOT NULL, v INT64, w INT64)");
    for (int i = 0; i < kRows; ++i)
      e.run_sql("INSERT INTO events VALUES (" + std::to_string(i) + ", " +
                std::to_string(rng() % kDomain) + ", " + std::to_string(rng() % 1000) + ")");
    e.run_sql("CREATE INDEX ON events (v)");
  }

  // ------------------------------------------------------ calibration
  double seq = 0, rnd = 0, miss = 0, seek = 0, build = 0, probe = 0, pair = 0;
  double seq11 = 0, rnd11 = 0;
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
    std::printf("\ncalibration: one step of a plan at a time (median of 15 rounds, per row)\n");
    std::printf("                                                          in batches              Unit 11's way\n");
    // the scan and the walk through the index, each in batches and Unit 11's way, timed in turn
    const auto drain_with = [&cat](Operator& op, std::size_t batch, bool copy) {
      return [&op, &cat, batch, copy] {
        cat.set_copy_pages(copy);
        drain(op, batch);
        cat.set_copy_pages(false);
      };
    };
    SeqScan scan(cat, TableRef{"events", ""}, schema);
    IndexScan through(cat, TableRef{"events", ""}, schema, 1, 0, ~Key{0}, "every v");
    std::vector<Timing> t = interleaved({drain_with(scan, kDefaultBatchRows, false), drain_with(scan, 0, true),
                                         drain_with(through, kDefaultBatchRows, false), drain_with(through, 0, true)},
                                        15);
    for (Timing& x : t) x = Timing{x.median * 1e9 / kRows, x.lo * 1e9 / kRows, x.hi * 1e9 / kRows};
    const Timing ts = t[0], ts11 = t[1], ti = t[2], ti11 = t[3];
    seq = ts.median, seq11 = ts11.median, rnd = ti.median, rnd11 = ti11.median;
    std::printf("  a row of a SeqScan, in key order                          %6.1f ns (%.1f-%.1f)   %6.1f ns\n", seq,
                ts.lo, ts.hi, seq11);
    std::printf("  a row through the index on v, in v's order, page in pool  %6.1f ns (%.1f-%.1f)   %6.1f ns\n", rnd,
                ti.lo, ti.hi, rnd11);

    // one search: seek to a key no row holds (v is drawn below kDomain)
    IndexCursor cur = cat.open_index("events", 1);
    std::vector<Key> probes;
    for (int i = 0; i < 100000; ++i)
      probes.push_back(order_key(Field::Int64(kDomain + static_cast<std::int64_t>(rng() % kDomain))));
    const Timing tk = timed([&] {
      Row r;
      for (Key k : probes) {
        cur.seek(k, k);
        cur.next(r);
      }
    }, 15);
    seek = tk.median * 1e9 / static_cast<double>(probes.size());
    std::printf("  one index search, root to leaf                            %6.0f ns\n", seek);
  }
  {
    // the same index walk through a pool that holds a sixth of the heap
    Pager pager(path);
    BufferPool pool(pager, 256);
    Catalog cat(pool);
    const OutSchema schema = table_schema(cat.schema_of("events"), TableRef{"events", ""});
    IndexScan through(cat, TableRef{"events", ""}, schema, 1, 0, ~Key{0}, "every v");
    drain(through, kDefaultBatchRows);
    pool.reset_stats();
    const Timing tm = timed([&] { drain(through, kDefaultBatchRows); }, 15);
    const double reads = static_cast<double>(pool.disk_reads()) / 16.0 / kRows;  // the warm run and 15
    const double per_row = tm.median * 1e9 / kRows;
    miss = (per_row - rnd) / reads;
    std::printf("  ... with a pool of 256 pages, %4.1f%% of rows read from the file  %6.0f ns\n", reads * 100,
                per_row);
    std::printf("      so a read from the file adds                          %6.0f ns\n", miss);
  }
  {
    // hash join and nested loop, over columns in memory, in batches
    const OutSchema ks = {{"a", {"k", ColType::kInt64, false}}, {"a", {"x", ColType::kInt64, false}}};
    const OutSchema rs = {{"b", {"k", ColType::kInt64, false}}, {"b", {"y", ColType::kInt64, false}}};
    std::vector<std::int64_t> seqv(kRows), fewk(1000), fewv(1000);
    for (int i = 0; i < kRows; ++i) seqv[static_cast<std::size_t>(i)] = i;
    for (int i = 0; i < 1000; ++i) fewk[static_cast<std::size_t>(i)] = static_cast<std::int64_t>(i) * kRows, fewv[static_cast<std::size_t>(i)] = i;
    const std::vector<ColumnVector> big{ints(seqv), ints(seqv)}, few{ints(fewk), ints(fewv)},
        none{ints({}), ints({})};
    const auto join_ns = [&](const std::vector<ColumnVector>& l, std::size_t nl, const std::vector<ColumnVector>& r,
                             std::size_t nr) {
      HashJoin hj(std::make_unique<ColumnsScan>(ks, l, nl), std::make_unique<ColumnsScan>(rs, r, nr), {{0, 0}}, {},
                  "a.k = b.k");
      return timed([&] { drain(hj, kDefaultBatchRows); }, 15).median * 1e9;
    };
    ColumnsScan only(ks, big, kRows);
    const double input = timed([&] { drain(only, kDefaultBatchRows); }, 15).median * 1e9 / kRows;
    build = join_ns(none, 0, big, kRows) / kRows - input;  // build 200,000, probe nothing
    probe = join_ns(big, kRows, few, 1000) / kRows - input;  // probe 200,000 past 1,000, no match
    std::vector<std::int64_t> l2(2000), r2(2000), r2v(2000);
    for (int i = 0; i < 2000; ++i) l2[static_cast<std::size_t>(i)] = i, r2[static_cast<std::size_t>(i)] = -1 - i, r2v[static_cast<std::size_t>(i)] = i;
    const std::vector<ColumnVector> lc{ints(l2), ints(l2)}, rc{ints(r2), ints(r2v)};
    NestedLoopJoin nl(std::make_unique<ColumnsScan>(ks, lc, 2000), std::make_unique<ColumnsScan>(rs, rc, 2000),
                      {BoundComparison{0, TokenType::kEq, false, false, true, 2, Field{}}}, "a.k = b.k");
    pair = timed([&] { drain(nl, kDefaultBatchRows); }, 15).median * 1e9 / (2000.0 * 2000.0);
    std::printf("  a row into a join's hash table                            %6.0f ns\n", build);
    std::printf("  a row probed against it                                   %6.0f ns\n", probe);
    std::printf("  a pair of rows a nested loop compares                     %6.1f ns\n", pair);
  }
  std::printf("the planner's constants (src/executor.hpp): %.0f, %.0f, %.0f, %.0f, %.0f, %.0f, %.1f ns\n",
              cost::kSeqRow, cost::kRandomRow, cost::kMissRow, cost::kSeek, cost::kBuildRow, cost::kProbeRow,
              cost::kPairRow);
  std::printf("this run measured:                          %.0f, %.0f, %.0f, %.0f, %.0f, %.0f, %.1f ns\n", seq,
              rnd, miss, seek, build, probe, pair);
  std::printf("the ratio a crossover follows, index row / scan row: %.1f in batches, %.1f Unit 11's way\n",
              rnd / seq, rnd11 / seq11);

  // ------------------------------------------- where batches start to pay
  // A plan run in batches costs more to start, and less for every row. The
  // executor runs a plan in batches only from cost::kBatchFrom estimated rows
  // on: this is where that number comes from. Between runs, an untimed
  // one-row query, so that tearing down the last plan is charged to neither.
  {
    Pager pager(path);
    BufferPool pool(pager, 8192);
    Catalog cat(pool);
    Executor e(cat);
    const long ks[] = {1, 4, 16, 64, 128, 256, 512, 1024, 4096};
    constexpr int kSweepRounds = 15;
    std::printf("\nwhere batches start to pay: SELECT SUM(w) FROM events WHERE id < K, a row at a time and in "
                "batches (us per query, median of %d rounds)\n\n", kSweepRounds);
    std::printf("        K   a row at a time   in batches   batches/rows\n");
    double from = -1, prev_k = 0, prev_ratio = 0;
    for (long k : ks) {
      const std::string sql = "SELECT SUM(w) FROM events WHERE id < " + std::to_string(k);
      std::vector<double> t[2];
      for (int r = 0; r <= kSweepRounds; ++r)  // round 0 only warms
        for (int b = 0; b < 2; ++b) {
          e.set_batch_rows(0);
          e.run_sql("SELECT w FROM events WHERE id = 0");
          e.set_batch_rows(b ? kDefaultBatchRows : 0);
          const double t0 = labdb::test::now_s();
          e.run_sql(sql);
          if (r) t[b].push_back(labdb::test::now_s() - t0);
        }
      for (auto& v : t) std::sort(v.begin(), v.end());
      const double rows = t[0][t[0].size() / 2] * 1e6, batches = t[1][t[1].size() / 2] * 1e6;
      const double ratio = batches / rows;
      if (from < 0 && ratio <= 1 && prev_ratio > 1)
        from = prev_k + (static_cast<double>(k) - prev_k) * (prev_ratio - 1) / (prev_ratio - ratio);
      prev_k = static_cast<double>(k);
      prev_ratio = ratio;
      std::printf("  %7ld  %15.1f  %11.1f  %12.2f\n", k, rows, batches, ratio);
    }
    e.set_batch_rows(Executor::kChoose);
    if (from > 0)
      std::printf("  batches start to pay at about %.0f rows; the executor's threshold (cost::kBatchFrom): %.0f\n",
                  from, cost::kBatchFrom);
    else
      std::printf("  no crossing in this range; the executor's threshold (cost::kBatchFrom): %.0f\n",
                  cost::kBatchFrom);
  }

  // ------------------------------------------------------------ the race
  const double sels[] = {0.0001, 0.001, 0.005, 0.01, 0.02, 0.03, 0.05, 0.07, 0.1, 0.15, 0.2, 0.3, 0.5, 0.7, 1.0};
  for (std::size_t frames : {std::size_t{8192}, std::size_t{256}}) {
    Pager pager(path);
    BufferPool pool(pager, frames);
    Catalog cat(pool);
    Executor e(cat);
    const double held = std::min(1.0, static_cast<double>(frames) / heap_pages);
    std::printf("\nSELECT SUM(w) FROM events WHERE v < X -- a pool of %zu pages, %s; ms, median of %d rounds\n",
                frames, held >= 1 ? "the whole heap fits" : "a sixth of the heap fits", kRaceRounds);
    std::printf("                       in batches (this unit)                         Unit 11's way\n");
    std::printf("  selectivity     rows  scan            index           chose     scan    index\n");
    double crossover = -1, crossover11 = -1;
    double prev_sel = 0, prev_ratio = 0, prev_ratio11 = 0;
    int right = 0, close = 0;
    for (double sel : sels) {
      const std::string sql = "SELECT SUM(w) FROM events WHERE v < " +
                              std::to_string(static_cast<std::int64_t>(sel * kDomain));
      // the four plans -- scan and index, in batches and Unit 11's way -- timed in turn
      const auto plan_run = [&e, &cat, &sql](Executor::AccessPath a, bool unit11) {
        return [&e, &cat, &sql, a, unit11] {
          e.set_access_path(a);
          cat.set_copy_pages(unit11);
          e.set_batch_rows(unit11 ? 0 : kDefaultBatchRows);
          e.run_sql(sql);
          cat.set_copy_pages(false);
          e.set_batch_rows(Executor::kChoose);
        };
      };
      const std::vector<Timing> t =
          interleaved({plan_run(Executor::AccessPath::kScan, false), plan_run(Executor::AccessPath::kIndex, false),
                       plan_run(Executor::AccessPath::kScan, true), plan_run(Executor::AccessPath::kIndex, true)},
                      kRaceRounds);
      const Timing scan = t[0], index = t[1], scan11 = t[2], index11 = t[3];
      e.set_access_path(Executor::AccessPath::kScan);
      e.run_sql(sql);
      const std::string plan = e.explain_last();
      const std::size_t rows = std::stoul(plan.substr(plan.find("rows=", plan.find("Filter")) + 5));
      e.set_access_path(Executor::AccessPath::kAuto);
      e.run_sql(sql);
      const bool chose_index = e.explain_last().find("IndexScan") != std::string::npos;
      const double ratio = index.median / scan.median, ratio11 = index11.median / scan11.median;
      if (crossover < 0 && ratio > 1 && prev_ratio > 0)
        crossover = prev_sel + (sel - prev_sel) * (1 - prev_ratio) / (ratio - prev_ratio);
      if (crossover11 < 0 && ratio11 > 1 && prev_ratio11 > 0)
        crossover11 = prev_sel + (sel - prev_sel) * (1 - prev_ratio11) / (ratio11 - prev_ratio11);
      prev_sel = sel;
      prev_ratio = ratio;
      prev_ratio11 = ratio11;
      const bool faster_index = ratio < 1;
      right += chose_index == faster_index;
      // a choice is "close" when the two plans' ranges overlap: the machine cannot tell them apart
      close += chose_index != faster_index && !(index.lo > scan.hi || scan.lo > index.hi);
      char sb[32];
      std::snprintf(sb, sizeof sb, "%g%%", sel * 100);
      std::printf("  %9s %8zu  %5.2f (%5.2f-%5.2f) %6.2f (%5.2f-%5.2f)  %-6s  %6.1f  %7.1f\n", sb, rows,
                  scan.median * 1e3, scan.lo * 1e3, scan.hi * 1e3, index.median * 1e3, index.lo * 1e3,
                  index.hi * 1e3, chose_index ? "index" : "scan", scan11.median * 1e3, index11.median * 1e3);
    }
    const double model = cost::kSeqRow / (cost::kRandomRow + (1 - held) * cost::kMissRow);
    std::printf("  in batches the index stops winning at %.1f%% of the rows; the planner's constants put it at %.1f%%\n",
                crossover * 100, model * 100);
    std::printf("  Unit 11's way, on this machine: at %.0f%%\n", crossover11 * 100);
    std::printf("  the planner chose the faster plan at %d of %zu selectivities", right, std::size(sels));
    if (close) std::printf(" (and %d more where the two could not be told apart)", close);
    std::printf("\n");
  }
  ::unlink(path);
  return 0;
}
