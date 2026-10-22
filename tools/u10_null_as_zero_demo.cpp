// THE BUG, preserved for study: an aggregate update without the NULL line.
//
// "A NULL's value is 0, and adding 0 to a sum changes nothing" -- so the
// line that skips NULL looks redundant, and the test that checks SUM passes
// without it. It is true that SUM does not change. COUNT(col) does: it now
// counts the NULL rows too. AVG divides the same sum by that larger count and
// comes out too low. MIN compares the NULL as if it were 0, keeps it, and
// returns NULL. MAX survives, on data where every value is above 0.
//
// The demo asks one question of a reviews table -- which products are rated
// best? -- through the engine's own plan, then through the same plan with the
// update function swapped for the buggy one, and then runs the two checks
// that tell them apart. It exits non-zero unless the engine passes both
// checks and the buggy plan fails both.
//
// build: see the Makefile (links the engine).
#include <unistd.h>

#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "buffer_pool.hpp"
#include "executor.hpp"
#include "operator.hpp"
#include "pager.hpp"
#include "show.hpp"

using namespace labdb;
using labdb::test::cell_text;
using labdb::test::print_table;

namespace {

// BUGGY: agg_update with its NULL line missing.
void update_null_as_zero(const AggSpec& a, AggState& s, const Field& v) {
  if (a.star) {
    ++s.count;
    return;
  }
  ++s.count;  // "a NULL is 0, and 0 changes nothing"
  switch (a.func) {
    case AggFunc::kSum:
    case AggFunc::kAvg: s.sum += v.i64; break;
    case AggFunc::kMin: if (s.count == 1 || compare_values(v, s.best) < 0) s.best = v; break;
    case AggFunc::kMax: if (s.count == 1 || compare_values(v, s.best) > 0) s.best = v; break;
    case AggFunc::kCount:
    case AggFunc::kNone: break;
  }
}

const char* kQuestion =
    "SELECT product, COUNT(*), COUNT(stars), SUM(stars), AVG(stars), MIN(stars), MAX(stars) "
    "FROM reviews GROUP BY product ORDER BY AVG(stars) DESC";

// kQuestion's plan, assembled by hand -- the same operators the planner
// builds for it -- so that the one function under suspicion can be swapped.
// reviews: (id, product, stars); the aggregate's output is product, #0..#5.
OpPtr question_plan(Catalog& cat, HashAggregate::UpdateFn update) {
  const auto agg = [](AggFunc f, bool star) { return AggSpec{f, star, star ? 0u : 2u, ColType::kInt32}; };
  std::vector<AggSpec> aggs = {agg(AggFunc::kCount, true), agg(AggFunc::kCount, false),
                               agg(AggFunc::kSum, false),  agg(AggFunc::kAvg, false),
                               agg(AggFunc::kMin, false),  agg(AggFunc::kMax, false)};
  OpPtr root = std::make_unique<HashAggregate>(
      std::make_unique<SeqScan>(cat, TableRef{"reviews", ""}), std::vector<std::size_t>{1},
      std::move(aggs),
      "GROUP BY product: COUNT(*), COUNT(stars), SUM(stars), AVG(stars), MIN(stars), MAX(stars)",
      update);
  return std::make_unique<Sort>(std::move(root), std::vector<SortKey>{{4, true}}, std::nullopt,
                                "AVG(stars) DESC");
}

ResultSet run_plan(Operator& root) {
  ResultSet r;
  r.columns = {"product", "COUNT(*)", "COUNT(stars)", "SUM(stars)", "AVG(stars)", "MIN(stars)", "MAX(stars)"};
  root.open();
  Row row;
  while (root.next(row)) r.rows.push_back(row);
  root.close();
  return r;
}

std::string top3(const ResultSet& r) {
  std::string s;
  for (std::size_t i = 0; i < 3 && i < r.rows.size(); ++i)
    s += (i ? ", " : "") + r.rows[i][0].text + " " + cell_text(r.rows[i][4]);
  return s;
}

// The two checks. (1) MIN(stars) may be NULL only when COUNT(stars) is 0.
// (2) COUNT(stars) must equal the number of the product's rows that pass
// `stars >= 1`, counted by COUNT(*) through a WHERE -- a different path
// through the engine, whose NULL rule (Challenge 9.1) no NULL satisfies.
std::size_t failures(const ResultSet& r, Executor& e, bool print) {
  const ResultSet rated =
      e.run_sql("SELECT product, COUNT(*) FROM reviews WHERE stars >= 1 GROUP BY product");
  std::size_t bad = 0;
  if (print) std::printf("  product  COUNT(stars)  via WHERE  MIN(stars)  verdict\n");
  for (const auto& row : r.rows) {
    std::int64_t through_where = 0;
    for (const auto& x : rated.rows)
      if (x[0].text == row[0].text) through_where = x[1].i64;
    const bool min_ok = !row[5].is_null || row[2].i64 == 0;
    const bool count_ok = row[2].i64 == through_where;
    bad += !min_ok + !count_ok;
    const std::string verdict = count_ok && min_ok ? "ok"
                                : !count_ok && !min_ok ? "COUNT wrong, MIN impossible"
                                : !count_ok ? "COUNT wrong" : "MIN impossible";
    if (print)
      std::printf("  %-8s %12lld %10lld  %-10s  %s\n", row[0].text.c_str(),
                  static_cast<long long>(row[2].i64), static_cast<long long>(through_where),
                  cell_text(row[5]).c_str(), verdict.c_str());
  }
  return bad;
}

}  // namespace

int main() {
  const char* path = "u10_null_as_zero_demo.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 256);
  Catalog cat(pool);
  Executor e(cat);
  e.run_sql("CREATE TABLE reviews (id INT64 NOT NULL, product TEXT, stars INT32)");

  // Per product: how many reviews, what share came from the old site (no
  // star rating: NULL), and the mean the rated ones are drawn around.
  struct Product {
    const char* name;
    int reviews;
    int old_pct;
    double mean;
  };
  const Product products[] = {{"mug", 2000, 70, 4.7},  {"kettle", 1200, 60, 4.6},
                              {"blender", 1500, 45, 4.4}, {"fan", 500, 2, 4.2},
                              {"toaster", 900, 5, 4.1},  {"desk", 800, 10, 4.0},
                              {"lamp", 600, 0, 3.9},     {"chair", 700, 20, 3.6}};
  std::mt19937_64 rng(1004);
  std::int64_t id = 0, nulls = 0;
  for (const Product& p : products)
    for (int i = 0; i < p.reviews; ++i) {
      std::string stars = "NULL";
      if (static_cast<int>(rng() % 100) >= p.old_pct) {
        // two neighbouring star values around the mean, and the odd 1-star review
        const int lo = static_cast<int>(p.mean);
        int s = lo + (static_cast<double>(rng() % 1000) / 1000.0 < p.mean - lo ? 1 : 0);
        if (rng() % 100 < 3) s = 1;
        stars = std::to_string(s);
      } else {
        ++nulls;
      }
      e.run_sql("INSERT INTO reviews VALUES (" + std::to_string(id++) + ", '" + p.name + "', " + stars + ")");
    }
  std::printf("\n%lld reviews of 8 products; %lld of them, imported from the old site, have no star rating (NULL)\n",
              static_cast<long long>(id), static_cast<long long>(nulls));
  const std::string q = kQuestion;
  const std::size_t cut = q.find(" FROM");
  std::printf("\nthe question:\n  %s\n  %s\n", q.substr(0, cut).c_str(), q.substr(cut + 1).c_str());

  // The engine, through SQL -- and the hand-built plan with the engine's own
  // update function, which must give the same answer, row for row.
  const ResultSet right = e.run_sql(kQuestion);
  OpPtr same = question_plan(cat, agg_update);
  const ResultSet same_rows = run_plan(*same);
  bool ok = same_rows.rows == right.rows;

  std::printf("\nthe engine's answer (agg_update, with the NULL line):\n");
  print_table(right);
  OpPtr buggy = question_plan(cat, update_null_as_zero);
  const ResultSet wrong = run_plan(*buggy);
  std::printf("\nthe same plan with the NULL line missing:\n");
  print_table(wrong);
  std::printf("\nits plan, after running -- the planner's plan, operator for operator:\n%s",
              plan_text(*buggy).c_str());

  std::printf("\nthe three best-rated products:\n");
  std::printf("  engine: %s\n", top3(right).c_str());
  std::printf("  buggy:  %s\n", top3(wrong).c_str());
  std::size_t sum_max_same = 0;
  for (const auto& r : right.rows)
    for (const auto& w : wrong.rows)
      if (r[0].text == w[0].text) sum_max_same += (r[3] == w[3]) && (r[6] == w[6]);
  std::printf("  SUM(stars) and MAX(stars) agree for %zu of 8 products; COUNT(stars), AVG and MIN do not\n",
              sum_max_same);

  std::printf("\nthe checks, against the buggy answer:\n");
  const std::size_t bad_wrong = failures(wrong, e, true);
  std::printf("\nthe checks, against the engine's answer:\n");
  const std::size_t bad_right = failures(right, e, true);
  std::printf("\nfailed checks: %zu for the buggy plan, %zu for the engine\n", bad_wrong, bad_right);

  ok = ok && sum_max_same == 8 && bad_right == 0 && bad_wrong > 0;
  ::unlink(path);
  std::printf("%s\n", ok ? "u10_null_as_zero_demo: the checks catch the bug, and the engine passes them"
                         : "u10_null_as_zero_demo: UNEXPECTED RESULT");
  return ok ? 0 : 1;
}
