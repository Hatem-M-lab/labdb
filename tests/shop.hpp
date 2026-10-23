#pragma once
// Unit 10's shop, for Unit 11's benchmarks: 20,000 customers in 12 cities, the
// first cities far more populous, and 200,000 orders over a year in which low
// customer ids order far more often than high ones and order sizes lean
// small. The same seed and the same statements as tests/u10_group_bench.cpp,
// so the same rows. Not part of the engine.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "executor.hpp"
#include "harness.hpp"

namespace labdb::test {

// The shop's statements, in order, handed to `run` one at a time -- so that
// Unit 14 can give exactly the same ones to SQLite, with its own two CREATE
// TABLEs in place of labdb's.
template <class Run>
inline void shop_statements(Run&& run, const std::string& customers_ddl, const std::string& orders_ddl) {
  std::mt19937_64 rng(1005);
  const char* cities[] = {"Cairo", "Lagos", "Lima", "Oslo", "Pune", "Quito",
                          "Riga", "Seoul", "Tunis", "Turin", "Vigo", "Zagreb"};
  run(customers_ddl);
  run("BEGIN");  // Unit 13: one transaction, so one commit, for the whole load
  for (int i = 0; i < 20000; ++i) {
    const std::uint64_t u = rng() % 1000;
    const int city = static_cast<int>(u * u * 12 / 1000000);  // skewed toward the first cities
    run("INSERT INTO customers VALUES (" + std::to_string(i) + ", 'name_" + std::to_string(i) + "', '" +
        cities[city] + "')");
  }
  run(orders_ddl);
  for (int i = 0; i < 200000; ++i) {
    const std::uint64_t u = rng() % 20000;
    const std::uint64_t customer = (u * u) / 20000;  // low ids order more often
    const std::uint64_t cents = 100 + (rng() % 1000) * (rng() % 1000) / 10;  // mostly small
    run("INSERT INTO orders VALUES (" + std::to_string(i) + ", " + std::to_string(customer) + ", " +
        std::to_string(cents) + ", " + std::to_string(1 + rng() % 365) + ")");
  }
  run("COMMIT");
}

inline void build_shop(Executor& e) {
  shop_statements([&](const std::string& sql) { e.run_sql(sql); },
                  "CREATE TABLE customers (id INT64 NOT NULL, name TEXT, city TEXT)",
                  "CREATE TABLE orders (oid INT64 NOT NULL, customer INT64, cents INT64, day INT32)");
}

// One query, planned as Unit 10 planned it (literal planning) and by Unit 11's
// planner, the two alternating run for run so that neither inherits a warmer
// machine than the other; the first round only warms. Between runs, an
// untimed one-row query: the executor keeps each query's plan for
// explain_last() until the next query replaces it, and tearing down a plan of
// hash tables is not the next query's cost. Both plans must return the same
// rows. Returns the median times, each with its fastest and slowest run, and
// leaves Unit 11's plan as the last one.
struct Race {
  double before_ms = 0, after_ms = 0;
  double before_lo = 0, before_hi = 0, after_lo = 0, after_hi = 0;
  ResultSet result;
};

inline std::string sorted_rows(const ResultSet& r) {
  std::vector<std::string> rows;
  for (const auto& row : r.rows) {
    std::string s;
    for (const Field& f : row) s += (f.is_null ? "NULL" : f.type == ColType::kText ? f.text : std::to_string(f.i64)) + "|";
    rows.push_back(s);
  }
  std::sort(rows.begin(), rows.end());
  std::string all;
  for (const auto& s : rows) all += s + "\n";
  return all;
}

inline Race race(Executor& e, const std::string& sql, int reps = 5) {
  Race out;
  ResultSet before;
  std::vector<double> tb, ta;
  for (int r = 0; r <= reps; ++r)
    for (bool literal : {true, false}) {
      e.set_literal_planning(literal);
      e.run_sql("SELECT id FROM customers WHERE id = 0");
      const double t0 = now_s();
      (literal ? before : out.result) = e.run_sql(sql);
      if (r > 0) (literal ? tb : ta).push_back(now_s() - t0);
    }
  if (sorted_rows(before) != sorted_rows(out.result)) std::abort();  // the plans disagree
  std::sort(tb.begin(), tb.end());
  std::sort(ta.begin(), ta.end());
  out.before_ms = tb[tb.size() / 2] * 1e3;
  out.after_ms = ta[ta.size() / 2] * 1e3;
  out.before_lo = tb.front() * 1e3;
  out.before_hi = tb.back() * 1e3;
  out.after_lo = ta.front() * 1e3;
  out.after_hi = ta.back() * 1e3;
  e.run_sql(sql);  // Unit 11's plan, once more, for explain_last()
  return out;
}

}  // namespace labdb::test
