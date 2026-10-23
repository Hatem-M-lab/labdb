// Challenge 12.4: grouping, sorting and joining in batches.
//
// Unit 10 left two measurements with a promise. Its cardinality experiment
// grouped 2,000,000 rows into 10 to 1,000,000 groups and found a group costing
// 224 bytes in five separate places, 96 of them for aggregates the query did
// not compute; its sort of 1,000,000 rows spent about 200 ns a row just
// handing the sorted rows out, one allocation per row. Here are both
// experiments again, each run the row way and the batch way over the same
// values -- rows (RowsScan) for the one, columns (ColumnsScan) for the other --
// and then a hash join, built from 20,000 rows and probed by 200,000, the
// shape of the shop's customers and orders. The two ways are timed in turn,
// round after round (tests/interleave.hpp). Medians of 7 rounds, with the
// fastest and slowest.
#include <algorithm>
#include <cstdio>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "executor.hpp"
#include "harness.hpp"
#include "interleave.hpp"
#include "operator.hpp"

using namespace labdb;
using labdb::test::do_not_optimize;
using labdb::test::interleaved;
using labdb::test::now_s;
using labdb::test::Timing;

namespace {

constexpr int kRounds = 7;

std::size_t drain(Operator& op, std::size_t batch) {
  op.set_batch_rows(batch);
  op.open();
  std::size_t n = 0;
  if (batch == 0) {
    Row r;
    while (op.next(r)) ++n;
    do_not_optimize(r);
  } else {
    Batch b;
    while (op.next_batch(b)) n += b.count();
  }
  op.close();
  return n;
}

// The same values as rows and as columns.
struct Data {
  std::vector<Row> rows;
  std::vector<ColumnVector> cols;
  void reset(const std::vector<ColType>& types, std::size_t n) {
    rows.assign(n, Row{});
    cols.assign(types.size(), ColumnVector{});
    for (std::size_t c = 0; c < types.size(); ++c) cols[c].type = types[c];
  }
  void set(std::size_t i, Row r) {
    for (std::size_t c = 0; c < r.size(); ++c) {
      if (cols[c].nulls.size() <= i) cols[c].push(r[c]);
      else cols[c].set(i, r[c]);
    }
    rows[i] = std::move(r);
  }
  OpPtr scan(const OutSchema& s, bool batches) const {
    if (batches) return std::make_unique<ColumnsScan>(s, cols, rows.size());
    return std::make_unique<RowsScan>(s, rows);
  }
};

}  // namespace

int main() {
  std::mt19937_64 rng(1204);

  // ---------------------------------------------- the cardinality experiment
  {
    const std::size_t kRows = 2'000'000;
    const OutSchema schema = {{"t", {"k", ColType::kInt64, false}}, {"t", {"v", ColType::kInt64, false}}};
    Data d;
    const auto fill = [&](std::size_t groups, bool clustered) {
      std::vector<std::int64_t> keys(kRows);
      for (std::size_t i = 0; i < kRows; ++i) keys[i] = static_cast<std::int64_t>(i % groups);
      if (clustered) std::sort(keys.begin(), keys.end());
      else std::shuffle(keys.begin(), keys.end(), rng);
      d.reset({ColType::kInt64, ColType::kInt64}, kRows);
      for (std::size_t i = 0; i < kRows; ++i)
        d.set(i, {Field::Int64(keys[i]), Field::Int64(static_cast<std::int64_t>(rng() % 1000))});
    };
    const auto ns = [&](double s) { return s * 1e9 / static_cast<double>(kRows); };
    std::printf("\nthe cardinality experiment: %zu in-memory rows, GROUP BY k: COUNT(*), SUM(v)\n", kRows);
    std::printf("(ns per row, median of %d rounds; a group's bytes as HashAggregate::bytes() counts them)\n\n",
                kRounds);
    fill(10, false);
    {
      const std::vector<Timing> t = interleaved(
          {[&] { OpPtr s = d.scan(schema, false); drain(*s, 0); },
           [&] { OpPtr s = d.scan(schema, true); drain(*s, kDefaultBatchRows); }},
          kRounds);
      std::printf("  for scale, the input alone: %.1f ns per row a row at a time, %.1f in batches\n",
                  ns(t[0].median), ns(t[1].median));
    }
    std::printf("\n                       a row at a time                     in batches\n");
    std::printf("     groups   per group   ns/row        (range)   per group   ns/row        (range)\n");
    const auto group = [&](std::size_t groups, bool clustered) {
      fill(groups, clustered);
      std::size_t bytes[2] = {0, 0};
      std::vector<std::function<void()>> runs;
      for (int b = 0; b < 2; ++b)
        runs.push_back([&, b] {
          HashAggregate agg(d.scan(schema, b == 1), {0},
                            {{AggFunc::kCount, true, 0, ColType::kInt64}, {AggFunc::kSum, false, 1, ColType::kInt64}},
                            "GROUP BY k: COUNT(*), SUM(v)");
          REQUIRE(drain(agg, b == 1 ? kDefaultBatchRows : 0) == groups);
          bytes[b] = agg.bytes();
        });
      const std::vector<Timing> t = interleaved(runs, kRounds);
      std::printf("  %9zu  %8.0f B  %7.1f  (%5.1f-%5.1f)  %8.0f B  %7.1f  (%5.1f-%5.1f)%s\n", groups,
                  static_cast<double>(bytes[0]) / static_cast<double>(groups), ns(t[0].median), ns(t[0].lo),
                  ns(t[0].hi), static_cast<double>(bytes[1]) / static_cast<double>(groups), ns(t[1].median),
                  ns(t[1].lo), ns(t[1].hi), clustered ? "  <- each group's rows together" : "");
    };
    for (std::size_t g : {10, 100, 1000, 10000, 100000, 1000000}) group(g, false);
    group(1000000, true);
  }

  // ---------------------------------------------------------------- sorting
  {
    const std::size_t kRows = 1'000'000;
    const OutSchema schema = {{"t", {"id", ColType::kInt64, false}},
                              {"t", {"key", ColType::kInt64, false}},
                              {"t", {"payload", ColType::kInt32, false}}};
    Data d;
    d.reset({ColType::kInt64, ColType::kInt64, ColType::kInt32}, kRows);
    for (std::size_t i = 0; i < kRows; ++i)
      d.set(i, {Field::Int64(static_cast<std::int64_t>(i)), Field::Int64(static_cast<std::int64_t>(rng() >> 1)),
                Field::Int32(static_cast<std::int32_t>(rng() % 1000))});
    std::printf("\nordering %zu in-memory rows by a random 64-bit key (ms, median of %d rounds)\n\n", kRows,
                kRounds);
    std::printf("                                a row at a time                  in batches\n");
    std::printf("  ORDER BY key          open()    next()     total      open()    next()     total\n");
    for (std::optional<std::size_t> limit :
         {std::optional<std::size_t>{}, std::optional<std::size_t>{10}, std::optional<std::size_t>{1000},
          std::optional<std::size_t>{100000}}) {
      std::vector<double> to[2], tn[2], tall[2];
      for (int r = 0; r <= kRounds; ++r)  // round 0 only warms
        for (int b = 0; b < 2; ++b) {
          Sort s(d.scan(schema, b == 1), {{1, false}}, limit, "key");
          s.set_batch_rows(b == 1 ? kDefaultBatchRows : 0);
          const double t0 = now_s();
          s.open();
          const double t1 = now_s();
          std::size_t k = 0;
          if (b == 0) {
            Row row;
            while (s.next(row)) ++k;
          } else {
            Batch bt;
            while (s.next_batch(bt)) k += bt.count();
          }
          const double t2 = now_s();
          s.close();
          REQUIRE(k == (limit ? std::min(*limit, kRows) : kRows));
          if (r == 0) continue;
          to[b].push_back(t1 - t0);
          tn[b].push_back(t2 - t1);
          tall[b].push_back(t2 - t0);
        }
      const auto med = [](std::vector<double>& v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2] * 1e3;
      };
      const std::string label = limit ? "LIMIT " + std::to_string(*limit) : "(all)";
      std::printf("  %-16s %8.1f  %8.1f  %8.1f    %8.1f  %8.1f  %8.1f\n", label.c_str(), med(to[0]), med(tn[0]),
                  med(tall[0]), med(to[1]), med(tn[1]), med(tall[1]));
    }
  }

  // ------------------------------------------------------------- a hash join
  {
    const std::size_t kBuild = 20000, kProbe = 200000;
    const char* cities[] = {"Cairo", "Lagos", "Lima", "Oslo", "Pune", "Quito",
                            "Riga", "Seoul", "Tunis", "Turin", "Vigo", "Zagreb"};
    const OutSchema cs = {{"c", {"id", ColType::kInt64, false}},
                          {"c", {"name", ColType::kText, true}},
                          {"c", {"city", ColType::kText, true}}};
    const OutSchema os = {{"o", {"oid", ColType::kInt64, false}},
                          {"o", {"customer", ColType::kInt64, true}},
                          {"o", {"cents", ColType::kInt64, true}}};
    Data c, o;
    c.reset({ColType::kInt64, ColType::kText, ColType::kText}, kBuild);
    for (std::size_t i = 0; i < kBuild; ++i) {
      const std::uint64_t u = rng() % 1000;
      c.set(i, {Field::Int64(static_cast<std::int64_t>(i)), Field::Text("name_" + std::to_string(i)),
                Field::Text(cities[u * u * 12 / 1000000])});
    }
    o.reset({ColType::kInt64, ColType::kInt64, ColType::kInt64}, kProbe);
    for (std::size_t i = 0; i < kProbe; ++i) {
      const std::uint64_t u = rng() % 20000;
      o.set(i, {Field::Int64(static_cast<std::int64_t>(i)), Field::Int64(static_cast<std::int64_t>(u * u / 20000)),
                Field::Int64(static_cast<std::int64_t>(100 + (rng() % 1000) * (rng() % 1000) / 10))});
    }
    std::printf("\na hash join in memory: %zu orders probing %zu customers (built from the customers), "
                "o.customer = c.id (ns per probe row, median of %d rounds)\n\n", kProbe, kBuild, kRounds);
    std::printf("  what is done with the joined rows        a row at a time   in batches\n");
    const auto join = [&](bool b, bool by_city) -> OpPtr {
      OpPtr hj = std::make_unique<HashJoin>(o.scan(os, b), c.scan(cs, b), std::vector<HashJoin::KeyPair>{{1, 0}},
                                            std::vector<BoundComparison>{}, "o.customer = c.id", mix64, false);
      if (!by_city)
        return std::make_unique<HashAggregate>(std::move(hj), std::vector<std::size_t>{},
                                               std::vector<AggSpec>{{AggFunc::kCount, true, 0, ColType::kInt64},
                                                                    {AggFunc::kSum, false, 2, ColType::kInt64}},
                                               "COUNT(*), SUM(o.cents)");
      return std::make_unique<HashAggregate>(std::move(hj), std::vector<std::size_t>{5},
                                             std::vector<AggSpec>{{AggFunc::kCount, true, 0, ColType::kInt64},
                                                                  {AggFunc::kSum, false, 2, ColType::kInt64}},
                                             "GROUP BY c.city: COUNT(*), SUM(o.cents)");
    };
    std::vector<std::function<void()>> runs;
    for (bool by_city : {false, true})
      for (int b = 0; b < 2; ++b)
        runs.push_back([&, by_city, b] {
          OpPtr op = join(b == 1, by_city);
          drain(*op, b == 1 ? kDefaultBatchRows : 0);
        });
    const std::vector<Timing> t = interleaved(runs, kRounds);
    const auto per = [&](const Timing& x) { return x.median * 1e9 / kProbe; };
    std::printf("  %-40s %14.1f %12.1f\n", "COUNT(*), SUM(o.cents)", per(t[0]), per(t[1]));
    std::printf("  %-40s %14.1f %12.1f\n", "grouped by c.city: COUNT(*), SUM(o.cents)", per(t[2]), per(t[3]));
  }
  return 0;
}
