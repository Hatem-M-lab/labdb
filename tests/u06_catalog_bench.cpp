// The catalog, measured. Registers several named tables, builds them up
// through nothing but Catalog::insert_row, and then looks rows up purely by
// (table name, key) through Catalog::get_by_key -- never touching a Heap or
// BTree object directly. This is the point of Unit 6: a query no longer
// needs to already hold a heap or a tree handle. It asks for a table by
// name, and the catalog does the rest.
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "buffer_pool.hpp"
#include "catalog.hpp"
#include "harness.hpp"
#include "pager.hpp"

using namespace labdb;
using labdb::test::now_s;

namespace {
Schema wide_schema() {
  return Schema{{
      {"id", ColType::kInt64, false},
      {"name", ColType::kText, true},
      {"score", ColType::kInt32, true},
  }};
}
Row row_for(const std::string& table, int i) {
  const std::size_t len = 5 + ((i + int(table.size())) % 18);
  return Row{Field::Int64(i),
             (i % 8 == 0) ? Field::Null(ColType::kText)
                          : Field::Text(table + "_" + std::string(len, 'a' + (i % 26))),
             (i % 6 == 0) ? Field::Null(ColType::kInt32) : Field::Int32(i * 3)};
}
}  // namespace

int main() {
  const char* path = "u06_catalog_bench.db";
  ::unlink(path);
  const std::vector<std::string> tables = {"accounts", "sessions", "events",
                                            "inventory", "shipments"};
  const int kRowsPerTable = 100000;
  const Schema s = wide_schema();

  // 5 tables x 100,000 rows costs more total pages than one table of the
  // same combined size would: each table plants its OWN B+Tree, so the
  // engine now carries five separate roots and internal-node layers instead
  // of one. Size the pool comfortably past that real total so this
  // benchmark measures the cost of the catalog's indirection -- one extra
  // page read, for the catalog page itself -- and not an eviction artifact
  // from an undersized cache (Unit 4's lesson, applied here too).
  Pager pager(path);
  BufferPool pool(pager, 16384);
  Catalog cat(pool);
  for (const auto& t : tables) cat.create_table(t, s);

  const double t0 = now_s();
  for (const auto& t : tables)
    for (int i = 0; i < kRowsPerTable; ++i) cat.insert_row(t, row_for(t, i));
  pool.sync();
  const double build_s = now_s() - t0;
  const std::uint32_t total_pages = pool.page_count();

  // Probe every table by name, random keys within each, purely through the
  // catalog's (table, key) interface.
  std::vector<std::pair<std::string, int>> probes;
  std::mt19937_64 rng(99);
  for (int k = 0; k < 300000; ++k) {
    const auto& t = tables[rng() % tables.size()];
    probes.emplace_back(t, static_cast<int>(rng() % kRowsPerTable));
  }

  pool.reset_stats();
  std::uint64_t mismatches = 0;
  const double t1 = now_s();
  for (auto& [t, key] : probes) {
    const auto got = cat.get_by_key(t, static_cast<Key>(key));
    if (!got) { ++mismatches; continue; }
    const Row want = row_for(t, key);
    if (!((*got)[0] == want[0] && (*got)[1] == want[1] && (*got)[2] == want[2]))
      ++mismatches;
  }
  const double look_s = now_s() - t1;
  REQUIRE(mismatches == 0);

  std::printf(
      "built %zu tables x %d rows = %zu rows through the catalog only, in "
      "%.2f s (%.0f rows/s), %u pages total (5 separate trees cost more "
      "than 1 tree over the same rows would).\n",
      tables.size(), kRowsPerTable, tables.size() * kRowsPerTable, build_s,
      tables.size() * kRowsPerTable / build_s, total_pages);
  std::printf(
      "\nlooked up %zu rows BY (TABLE NAME, KEY) -- catalog lookup, tree "
      "search, heap fetch, decode -- 0 mismatches, at %.0f lookups/s (%.0f "
      "ns each), %.2f logical reads/lookup (%.2f from disk).\n",
      probes.size(), probes.size() / look_s, look_s / probes.size() * 1e9,
      double(pool.read_count()) / probes.size(),
      double(pool.disk_reads()) / probes.size());

  ::unlink(path);
  return 0;
}
