// The milestone, measured: the whole engine answering SQL, end to end. Builds
// a database through SQL, runs a session of representative queries with their
// results printed as tables, and then measures query throughput -- parse,
// plan, execute, fetch -- for both an indexed point lookup and a full scan.
// Everything below is a SQL string; the seven units underneath do the rest.
#include <unistd.h>

#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "buffer_pool.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "pager.hpp"

using namespace labdb;
using labdb::test::now_s;

namespace {

void print_table(const ResultSet& r, std::size_t max_rows = 6) {
  if (r.columns.empty()) return;
  std::string header;
  for (std::size_t i = 0; i < r.columns.size(); ++i)
    header += (i ? " | " : "") + r.columns[i];
  std::printf("    %s\n", header.c_str());
  for (std::size_t k = 0; k < r.rows.size() && k < max_rows; ++k) {
    std::string line;
    for (std::size_t i = 0; i < r.rows[k].size(); ++i) {
      const Field& f = r.rows[k][i];
      std::string cell;
      if (f.is_null) cell = "NULL";
      else if (f.type == ColType::kText) cell = f.text;
      else if (f.type == ColType::kBool) cell = f.i64 ? "TRUE" : "FALSE";
      else cell = std::to_string(f.i64);
      line += (i ? " | " : "") + cell;
    }
    std::printf("    %s\n", line.c_str());
  }
  if (r.rows.size() > max_rows)
    std::printf("    ... (%zu rows total)\n", r.rows.size());
}

void show(Executor& e, const std::string& sql) {
  std::printf("  SQL> %s\n", sql.c_str());
  ResultSet r = e.run_sql(sql);
  if (!r.columns.empty()) print_table(r);
}

}  // namespace

int main() {
  const char* path = "u08_sql_bench.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 8192);
  Catalog cat(pool);
  Executor e(cat);

  // ---- build a database through SQL ----
  e.run_sql("CREATE TABLE customers (id INT64 NOT NULL, name TEXT, vip BOOL)");
  e.run_sql("CREATE TABLE orders (oid INT64 NOT NULL, customer INT32 NOT NULL, cents INT32 NOT NULL)");
  const int kCustomers = 20000, kOrders = 200000;
  const double tb = now_s();
  for (int i = 0; i < kCustomers; ++i)
    e.run_sql("INSERT INTO customers VALUES (" + std::to_string(i) + ", 'name_" +
              std::to_string(i) + "', " + (i % 10 == 0 ? "TRUE" : "FALSE") + ")");
  for (int i = 0; i < kOrders; ++i)
    e.run_sql("INSERT INTO orders VALUES (" + std::to_string(i) + ", " +
              std::to_string(i % kCustomers) + ", " + std::to_string((i * 7) % 100000) + ")");
  pool.sync();
  const double build_s = now_s() - tb;

  // ---- a session of representative queries ----
  std::printf("a SQL session against %d customers and %d orders:\n\n",
              kCustomers, kOrders);
  show(e, "SELECT id, name, vip FROM customers WHERE id = 12345");
  show(e, "SELECT name FROM customers WHERE vip <> FALSE");   // scan + filter
  show(e, "SELECT oid, cents FROM orders WHERE cents < 50");
  show(e, "SELECT oid FROM orders WHERE customer = 7");

  // ---- throughput: indexed point lookups ----
  std::mt19937_64 rng(8);
  const int kQ = 100000;
  double t0 = now_s();
  std::size_t hits = 0;
  for (int k = 0; k < kQ; ++k) {
    const int id = rng() % kCustomers;
    hits += e.run_sql("SELECT name FROM customers WHERE id = " + std::to_string(id)).row_count();
  }
  const double point_s = now_s() - t0;
  REQUIRE(hits == static_cast<std::size_t>(kQ));

  // ---- throughput: a full-scan aggregate-style query, repeated ----
  const int kScans = 200;
  t0 = now_s();
  std::size_t scan_rows = 0;
  for (int k = 0; k < kScans; ++k)
    scan_rows += e.run_sql("SELECT oid FROM orders WHERE cents >= 99900").row_count();
  const double scan_s = now_s() - t0;

  std::printf(
      "\nbuilt %d rows through SQL in %.2f s (%.0f INSERTs/s).\n",
      kCustomers + kOrders, build_s, (kCustomers + kOrders) / build_s);
  std::printf(
      "indexed point lookups (parse+plan+index+fetch): %d queries in %.2f s, "
      "%.0f queries/s (%.1f us each).\n",
      kQ, point_s, kQ / point_s, point_s / kQ * 1e6);
  std::printf(
      "full-scan filter over %d orders: %d scans in %.2f s, %.0f rows "
      "scanned/s (each scan returned %zu rows).\n",
      kOrders, kScans, scan_s, double(kScans) * kOrders / scan_s,
      scan_rows / kScans);

  ::unlink(path);
  return 0;
}
